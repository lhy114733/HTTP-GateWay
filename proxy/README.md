# proxy/ —— 一次代理请求的事务层

`ProxySession` 把下游和上游接起来：

```text
下游请求
  → ProxySession
    → HttpClientSession → 上游
    → 上游 headers/body
      → HttpResponseWriter → 下游
```

它不属于 HTTP codec，也不属于路由/鉴权策略；它是**业务层的数据面**。
网关鉴权和上游 key 注入在 `../gateway/`。

## 文件

- `ports.hpp`：`UpstreamClient` / `DownstreamSink` / `TaskRunner` 三个端口
- `proxy_session.hpp`：一次代理请求的状态机，只依赖 `ports.hpp` + `http_message.hpp`
- `proxy_manager.hpp`：持有活跃 ProxySession，管理生命周期
- `../example/proxy_server_test.cc`：最小代理服务端示例
- `../example/stunnel-deepseek.conf`：TLS sidecar 配置示例
- `../gateway/`：网关 key 校验和 provider key 注入

适配器在 `adapter/`：

- `http_client_upstream.hpp`：`HttpClientSession` → `UpstreamClient`
- `http_response_sink.hpp`：`HttpResponseWriter` → `DownstreamSink`
- `eventloop_task_runner.hpp`：`Eventloop::QueueInLoop` → `TaskRunner`

## 编译和运行

代理启动前必须配置两个 key：

```bash
export GATEWAY_API_KEY='gw-local-dev'     # 客户端访问网关用的 key
export DEEPSEEK_API_KEY='sk-...'          # 网关访问 provider 用的 key
```

本地测试没有真实 provider key 时，可以用一个占位值；
本地上游不会校验它：

```bash
export DEEPSEEK_API_KEY='sk-local-dummy'
```

先启动一个上游：

```bash
cd adapter
make
./build/http_server_test 8080
```

再启动代理：

```bash
cd proxy
make
./build/http_proxy_test 8081 8080
```

也可以直接传上游域名，代理启动时会解析一次：

```bash
./build/http_proxy_test 8081 8080 localhost
./build/http_proxy_test 8081 80 api.example.com
```

请求必须带网关 key：

```bash
curl -H "Authorization: Bearer $GATEWAY_API_KEY" \
  http://127.0.0.1:8081/healthz

curl -H "Authorization: Bearer $GATEWAY_API_KEY" \
  -X POST -d '{"a":1}' http://127.0.0.1:8081/echo
```

没有 key 或 key 错误会返回 401，不会连接上游。

## 接 HTTPS 上游：stunnel

C++ 侧仍然只发明文 HTTP，TLS 由 stunnel 完成：

```bash
# 终端 1：本地 18443 -> api.deepseek.com:443，做 TLS
stunnel4 ../example/stunnel-deepseek.conf

# 终端 2：Host 用 api.deepseek.com:443，TCP 连本地 18443
./build/http_proxy_test 8081 443 api.deepseek.com 127.0.0.1 18443
```

参数依次是：

```text
代理端口 上游 Host 端口 上游域名 连接 IP 连接端口
```

也就是：

- HTTP `Host: api.deepseek.com`
- TCP `connect` 到 `127.0.0.1:18443`
- stunnel 负责 SNI、证书校验和真正的 TLS
- 网关负责把客户端的 `gw-...` 替换成 provider 的 `sk-...`

真实调用：

```bash
curl -N -H "Authorization: Bearer $GATEWAY_API_KEY" \
  -H 'Content-Type: application/json' \
  http://127.0.0.1:8081/v1/chat/completions \
  -d '{"model":"deepseek-chat","messages":[{"role":"user","content":"hi"}],"stream":true}'
```

客户端只发网关 key，不会看到 `DEEPSEEK_API_KEY`。

## 使用方式

鉴权在 `HttpServer` 的 async handler 里完成，然后才创建 `ProxySession`：

```cpp
gateway::Authenticator authenticator;
authenticator.AddKey(gateway_config.gateway_key,
                     gateway::Tenant{gateway_config.gateway_tenant});

server.SetAsyncHandler(
    [&manager, &authenticator, gateway_config](
        const http::HttpRequest& request,
        std::shared_ptr<http::HttpResponseWriter> writer,
        const std::shared_ptr<Connection>& conn) -> std::function<void()> {
        const gateway::AuthResult auth = authenticator.Authenticate(request);
        if (!auth.ok)
        {
            // 写 401，不创建 ProxySession
            return {};
        }

        // 网关 key 不进入 ProxySession
        http::HttpRequest upstream_request = request;
        upstream_request.headers.Remove("authorization");

        auto tasks = std::make_shared<EventloopTaskRunner>(conn->GetLoop());
        auto sink = std::make_shared<HttpResponseSink>(std::move(writer));
        auto upstream = std::make_unique<HttpClientUpstream>(
            conn->GetLoop(),
            "api.deepseek.com", 443,   // Host 头
            "127.0.0.1", 18443);       // 实际 TCP 连接

        upstream->SetUpstreamAuth(
            "Authorization", "Bearer " + gateway_config.provider_key);

        auto session = manager.Start(std::move(upstream), sink,
                                     std::move(upstream_request), tasks);

        return [session, tasks]() { session->Cancel(); };
    });
```

`HttpClientSession` 发送前还会再删一次 `Authorization` / `Proxy-Authorization`，
然后写入 provider 认证头，作为第二道防线。

`HttpServer` 的异步 handler 只负责鉴权、组装端口和创建 `ProxySession`；
`ProxyManager` 持有活跃 session，`ProxySession` 只依赖端口，不认识 TCP 和 key。

## 当前行为

- 下游请求先做网关鉴权，失败直接 401，不连接上游
- 客户端 `Authorization` 不转发给上游
- 上游使用配置里的 provider key
- hop-by-hop header 会被过滤
- 上游响应头先写下游，再流式转发 body
- 上游 `Content-Length` → 下游固定长度
- 上游 chunked / EOF → 下游 chunked
- 上游连接失败 → 下游 502
- 响应结束调用 `HttpResponseWriter::Finish()`，并通知 `HttpServerSession`
- `ProxyManager` 持有活跃 session，handler 返回后不会提前析构
- 下游断开时，`HttpServerSession` 调取消回调 → `ProxySession::Cancel`
  → `HttpClientUpstream::Cancel` → `TcpClient::Abort`，立即停止上游

## 手动验证

```bash
make
GATEWAY_API_KEY=gw-test DEEPSEEK_API_KEY=sk-provider-test \
  ./build/http_proxy_test 8081 8080

curl -H 'Authorization: Bearer gw-test' http://127.0.0.1:8081/auth
# Bearer sk-provider-test

curl -H 'Authorization: Bearer wrong' http://127.0.0.1:8081/auth
# 401
```

## 限制

- 只有一个网关 key，配置来自环境变量
- key 明文存在内存里，还没有哈希存储
- 上游域名只在启动时解析一次，地址变化后需要重启
- 只使用解析结果中的第一个 IPv4，没有多地址故障转移
- C++ 侧没有 TLS；HTTPS 上游需要 stunnel/nginx 这类 sidecar
- 没有首 token / idle 超时
- 没有背压：`Connection::Send` 目前无上限
- 没有路由、限流、配额和用量统计
- 一条上游请求一条连接，没有连接池
