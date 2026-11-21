# gateway/ —— 网关控制面

这一层负责“谁可以调用网关”和“网关用什么身份访问上游”，不参与 HTTP 数据转发。

```text
客户端
  Authorization: Bearer gw-xxx
        ↓
gateway::Authenticator
  校验网关 key，得到 tenant
        ↓
删除客户端 Authorization
        ↓
HttpClientSession::SetUpstreamAuth
  注入 Authorization: Bearer sk-provider-xxx
        ↓
stunnel -> provider
```

`proxy/` 仍然只负责一次请求的数据面事务，不认识网关 key。

## 文件

- `auth.hpp`：`Authenticator`、`Tenant`、`AuthResult`，解析 `Authorization: Bearer ...` 并做常量时间比较
- `config.hpp`：从环境变量加载最小配置

这两个文件都是 header-only，由 `proxy/` 编译时直接包含。

## 配置

MVP 先使用环境变量，不引入 YAML/JSON 解析器：

```bash
export GATEWAY_API_KEY='gw-local-dev'
export GATEWAY_TENANT='local'
export DEEPSEEK_API_KEY='sk-...'
```

- `GATEWAY_API_KEY`：客户端访问网关用的 key
- `GATEWAY_TENANT`：可选，默认 `default`
- `DEEPSEEK_API_KEY`：网关访问 provider 用的 key

两个 key 都缺失或为空时，代理启动失败，不会退化成“无鉴权放行”。

## 鉴权

客户端请求：

```http
Authorization: Bearer gw-local-dev
```

网关检查：

- header 缺失 → 401
- scheme 不是 Bearer → 401
- key 不存在 → 401
- key 有效 → 继续转发

401 响应：

```json
{"error":{"message":"invalid api key","type":"authentication_error"}}
```

## 上游 key 注入

`HttpClientSession` 在发送前会：

1. 删除客户端带来的 `Authorization`
2. 删除 `Proxy-Authorization`
3. 写入配置里的 provider 认证头

所以 provider 永远看不到网关 key，客户端也永远看不到 provider key。

## 手动验证

启动本地上游：

```bash
./adapter/build/http_server_test 8080
```

启动网关：

```bash
GATEWAY_API_KEY=gw-test DEEPSEEK_API_KEY=sk-provider-test \
  ./proxy/build/http_proxy_test 8081 8080 127.0.0.1
```

验证 key 注入：

```bash
curl -H 'Authorization: Bearer gw-test' http://127.0.0.1:8081/auth
# Bearer sk-provider-test

curl -H 'Authorization: Bearer wrong' http://127.0.0.1:8081/auth
# 401
```

## 当前限制

- 只支持一个网关 key
- 配置来自环境变量，还没有配置文件解析
- key 以明文形式存在内存里，还没有哈希存储
- 没有 per-tenant 限流、配额、模型白名单
- 只支持 `Authorization: Bearer`，Azure 的 `api-key` 等后续再抽象
