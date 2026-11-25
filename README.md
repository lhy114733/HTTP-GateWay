# 高并发 HTTP 网关（自研 Reactor 网络框架）

一个不依赖任何网络库的 C++ HTTP/1.1 网关：从 epoll 开始自己搭事件驱动框架，在它之上实现
下游 HTTP 服务端、上游 HTTP 客户端和流式转发，最终做成一个能把客户端密钥换成上游密钥、
把大模型流式响应原样透传给客户端的 AI 网关。

整个项目只依赖 libc 和 `picohttpparser`（仅用于请求行/头解析），
没有用 muduo、libevent 或任何 HTTP 框架。

## 为什么自己写

想搞清楚三件事：

1. `epoll` 从 `epoll_create` 到事件分发的完整链路，以及 `Channel` / `Eventloop` / `Poller`
   分层各自解决什么问题。
2. 一条连接从 `accept` 到关闭，对象什么时候该活着、什么时候该被销毁——这是 C++ 网络编程
   最容易出 bug 的地方（见 `docs/connection-lifetime-and-smart-pointers.md`）。
3. HTTP/1.1 报文边界：`Content-Length`、`chunked`、读到 EOF 三种结束方式在增量解析里
   分别怎么处理，chunk 分片落在 TCP 包边界上怎么办。

## 架构

```text
客户端
  │  HTTP/1.1 + Authorization: Bearer gw-xxx
  ▼
┌──────────────────── HttpServer（下游） ────────────────────┐
│  Acceptor ──► TcpServer ──► Connection ──► HttpServerSession│
│    (baseloop)   (subLoop)     (Buffer)      (增量解析请求)   │
└────────────────────────────┬───────────────────────────────┘
                             │ gateway::Authenticator 校验网关 key
                             ▼
                      ProxySession（一次代理事务）
                             │ 剥离客户端 Authorization，注入 provider key
                             ▼
┌──────────────────── HttpClientSession（上游） ─────────────┐
│  TcpClient ──► Connection ──► HttpResponseParser（增量解析）│
└────────────────────────────┬───────────────────────────────┘
                             │ stunnel（TLS sidecar）
                             ▼
                     上游 provider（如 api.deepseek.com）
```

## 目录

```text
core/net/          Reactor 框架：Poller / Channel / Eventloop / TimerWheel /
                   Connection / Acceptor / TcpServer / TcpClient / LoopThreadPool
core/http/         HTTP 编解码：请求解析、响应解析、请求/响应序列化、报文模型
core/comm/         日志与工具
adapter/downstream 下游适配：HttpServer / HttpServerSession
adapter/upstream   上游适配：HttpClientSession
adapter/runtime    启动期 DNS 解析（运行期不调 getaddrinfo）
gateway/           控制面：网关 key 校验、上游 key 注入、配置加载
proxy/             数据面：ProxySession（一次代理事务）、ProxyManager（生命周期）
example/           可运行示例与 stunnel 配置
docs/              设计文档
```

## 核心实现

- **事件驱动框架**：`Poller` 封装 `epoll_create/ctl/wait`，`Channel` 抽象「fd + 监听事件 +
  回调」，`Eventloop` 负责事件分发与任务队列，`LoopThreadPool` 提供主从 Reactor，
  主线程 `accept` 后轮询把新连接分发到 subLoop。
- **跨线程任务投递**：用 `eventfd` 唤醒阻塞在 `epoll_wait` 的 loop，实现 `RunInLoop` /
  `QueueInLoop` 双语义——前者同线程内联执行，后者永远排队，专门给「回调里可能把自己
  销毁掉」的场景用。
- **连接生命周期**：`Release()` 必须经 `QueueInLoop` 执行，不能在 `Channel::HandleEvent`
  的栈帧里析构自己；排队任务捕获 `shared_ptr` 自持，避免 `TcpClient` 提前释放连接。
  四态状态机 `CONNECTING → CONNECTED → DISCONNECTING → DISCONNECTED`，支持半关闭。
- **非活跃连接回收**：单层时间轮（60 槽 × 1s，`timerfd` 驱动），插入与刷新 O(1)；
  取消时只置标记、由任务析构回调删除索引，避免同 id 重建时误删新索引。
- **HTTP/1.1 增量解析**：请求侧复用 `picohttpparser` 抵御不可信输入；响应侧自研解析器，
  支持 `Content-Length`、`chunked`、读到 EOF 三种边界，chunk 分片可跨 TCP 包。
- **流式转发**：上游响应头到达即写下游，body 按 `chunked` / 固定长度边收边转，
  适配大模型流式输出；过滤 hop-by-hop 头。
- **密钥隔离**：客户端只持有网关 key，转发前剥离 `Authorization` 并注入 provider key，
  两道防线（handler 一次、`HttpClientSession` 一次）；key 比较用常量时间实现。
- **TLS 交给 sidecar**：C++ 侧只发明文 HTTP，由 stunnel 完成 SNI、证书校验与 TLS。

## 构建与运行

只依赖 `g++`（C++17）和 `make`。

```bash
# 起一个本地上游
cd adapter && make && ./build/http_server_test 8080

# 起网关（两个 key 必须配置，缺失会启动失败，不会退化成无鉴权）
cd proxy && make
GATEWAY_API_KEY=gw-test DEEPSEEK_API_KEY=sk-dummy \
  ./build/http_proxy_test 8081 8080
```

```bash
curl -H 'Authorization: Bearer gw-test' http://127.0.0.1:8081/healthz
# {"ok":true}

curl -H 'Authorization: Bearer gw-test' http://127.0.0.1:8081/auth
# Bearer sk-dummy      ← provider key 已注入，客户端 key 未透传

curl http://127.0.0.1:8081/healthz
# 401
```

接真实 HTTPS 上游时用 stunnel（见 `example/stunnel-deepseek.conf`）：

```bash
stunnel4 ../example/stunnel-deepseek.conf
./build/http_proxy_test 8081 443 api.deepseek.com 127.0.0.1 18443
```

## 当前限制

- 单网关 key，配置来自环境变量，key 明文存在内存里
- 上游一条请求一条连接，没有连接池
- `Connection::Send` 没有背压上限：下游客户端慢、上游持续输出时会撑大输出缓冲
- 上游域名只在启动时解析一次，只取第一个 IPv4，没有故障转移
- 没有限流、配额、路由与用量统计
