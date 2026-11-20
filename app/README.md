# app/ —— HTTP 与 TCP 的适配层

`core/http/` 下的 codec 不认识 socket；`core/net/` 下的 TCP 层不认识 HTTP。
这一层同时看到两者，负责接线：

```
TcpServer（core/net/）
    ↓ 连接事件 + Buffer
HttpServerSession（app/）
    ↓ 字节 / HttpRequest
http::HttpRequestParser / http::HttpResponseWriter（core/http/）
```

## 文件

- `http_server_session.hpp`：一条 TCP 连接对应一个 Session，负责解析请求、调用 handler、写响应。
- `http_server.hpp`：持有 `TcpServer`，按连接 id 管理 Session。
- `host_resolver.hpp`：启动阶段用 `getaddrinfo` 解析一次域名，得到 IPv4 列表。
- `http_client_session.hpp`：`TcpClient` + 请求序列化 + 响应解析，一个请求一条连接。
- `http_client_upstream.hpp`：把 `HttpClientSession` 适配成 `proxy::UpstreamClient`。
- `http_response_sink.hpp`：把 `HttpResponseWriter` 适配成 `proxy::DownstreamSink`。
- `eventloop_task_runner.hpp`：把 `Eventloop::QueueInLoop` 适配成 `proxy::TaskRunner`。
- `../example/http_server_test.cc`：最小服务端示例，提供 `/healthz`、`/echo` 和 `/auth`。
- `../example/stunnel-deepseek.conf`：TLS sidecar 配置示例。
- `../gateway/README.md`：网关鉴权和上游 key 注入。

## 编译和运行

```bash
cd app
make
./build/http_server_test 8080      # 单 loop
./build/http_server_test 8080 2    # 2 个从属 loop
```

或者：

```bash
make run
```

## 手动验证

```bash
curl -v --http1.1 http://127.0.0.1:8080/healthz
curl -v -X POST -H 'Content-Type: application/json' \
     -d '{"a":1}' http://127.0.0.1:8080/echo
curl -v http://127.0.0.1:8080/healthz http://127.0.0.1:8080/healthz
```

## 域名解析

`TcpClient` / `Connector` 仍然只接受数字 IPv4。域名在进程启动时解析一次：

```cpp
std::vector<std::string> addresses;
std::string error;
if (!HostResolver::ResolveOnce("api.example.com", 80, addresses, &error))
{
    // 启动失败，打印 error 后退出
}
const std::string connect_ip = addresses.front();
```

之后每个请求复用 `connect_ip`：

```cpp
HttpClientSession client(loop, "api.example.com", 80, connect_ip, 80);
```

- `host` / `host_port` 用于写 HTTP `Host` 头
- `connect_ip` / `connect_port` 只用于 TCP `connect`
- 运行期不再调用 `getaddrinfo`，不会阻塞 Eventloop

当前只保留 IPv4；`getaddrinfo` 的 `AF_INET6` 结果会被过滤掉。

## TLS sidecar

C++ 侧不实现 TLS。需要 HTTPS 时，用 stunnel 在本地做明文到 TLS 的转换：

```bash
stunnel4 ../example/stunnel-deepseek.conf
```

代理连接本地 stunnel：

```cpp
HttpClientSession client(loop,
                         "api.deepseek.com", 443,   // Host 头
                         "127.0.0.1", 18443);       // 实际 TCP 连接
```

此时 `Host` 是 `api.deepseek.com`，TCP 连的是 `127.0.0.1:18443`，
TLS、SNI 和证书校验都由 stunnel 完成。

## 上游认证

`HttpClientSession` 发送前会删除客户端的 `Authorization` 和
`Proxy-Authorization`，再写入配置的上游认证头：

```cpp
client.SetUpstreamAuth("Authorization", "Bearer " + provider_key);
```

如果 value 为空，就只删除、不注入。这样网关 key 不会被透传给 provider。
具体鉴权流程见 `../gateway/README.md`。

## handler

```cpp
void HandleRequest(const http::HttpRequest& req,
                   http::HttpResponseWriter& writer)
{
    // 只能通过 writer 写响应，不要直接碰 Connection
}
```

当前 handler 分两种：

- `SetHandler`：同步，返回前要把响应写完
- `SetAsyncHandler`：把 `shared_ptr<HttpResponseWriter>` 交给异步对象（例如 `ProxySession`），返回一个取消回调；连接关闭时会调用它

SSE 这类异步流式响应用 `SetAsyncHandler`。

## 注意

- `HttpServerSession` 会在解析错误后清空 `Buffer` 再 `Shutdown`，避免 `Connection::Shutdown` 把坏数据再次喂回 parser。
- `SetHandler()` 要在 `Start()` 之前调用；`Start()` 会阻塞。
- `HttpServer` 的 Session map 有 mutex；不同连接可能在不同 loop 上，`handler` 本身需要线程安全。
- `HttpClientSession` 是“一条连接一个请求”，默认发 `Connection: close`。
- `HttpClientSession` 要活到 `SetClosedCallback` 触发；不要在响应回调里立刻析构它。
- `HttpClientSession` 的 `host` / `host_port` 只用于 `Host` 头；连接地址是 `connect_ip` / `connect_port`。
- `HttpClientSession::SetUpstreamAuth` 会先删除客户端认证头，再注入 provider 认证头。
- 域名解析只在启动阶段做一次，地址变化后需要重启进程。
- `TcpClient` 仍然只接受数字 IP。
- C++ 侧没有 TLS；HTTPS 上游用 stunnel sidecar，见 `../example/stunnel-deepseek.conf`。
- 网关鉴权和 key 注入见 `../gateway/README.md`。
- 代理见 `../proxy/README.md`。
- 还没有连接池、超时和限流。
