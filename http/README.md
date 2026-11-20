# http/ —— 简化版 HTTP 解析

这里用的是 **picohttpparser v1.5**，pico 本身只有两个文件：

```
http/
├── picohttpparser.c
├── picohttpparser.h
├── http_message.hpp          # HeaderMap / HttpRequest / HttpResponse / 错误码
├── http_request_parser.hpp   # 下游请求解析状态机
├── http_response_writer.hpp  # 下游响应序列化
├── http_request_writer.hpp   # 上游请求序列化
├── http_response_parser.hpp  # 上游响应解析
├── http_codec_test.cc        # 请求解析测试
├── http_response_writer_test.cc
├── http_request_writer_test.cc
├── http_response_parser_test.cc
├── example.cc                # pico 原始示例
├── Makefile
└── README.md
```

- 上游：<https://github.com/h2o/picohttpparser>
- 版本：`v1.5`（tag `v1.5`）
- 许可：MIT 或 Perl 双许可（完整 MIT 文本在 `picohttpparser.h` 顶部）
- 大小：`picohttpparser.c` 约 28KB、`picohttpparser.h` 约 4KB

它比 llhttp 简单的地方：

- 没有 `llhttp_settings_t`、没有一堆回调。
- 不是生成代码。
- 不需要每个连接维护 parser 对象；它是**无状态**的，每次把缓冲区传进去就行。
- 只做两件事：解析 header、解码 chunked body。

## 编译

```bash
gcc -O2 -c http/picohttpparser.c -o http/picohttpparser.o
g++ -std=c++17 -O2 -I http http/example.cc http/picohttpparser.o -o http/example
./http/example
```

输出应该是：

```
[ OK ] request: POST /v1/chat/completions, body={"model":"x"}
[ OK ] response: 200, chunked body="data: x"

PASS
```

## 纯 HTTP codec（第一步）

`http_message.hpp` + `http_request_parser.hpp` 是与 TCP 完全解耦的请求解析层：

- 不 include `Connection`、`Buffer`、`TcpServer`、`TcpClient`
- 输入 `std::string_view`，输出 `http::HttpRequest`
- 支持 `Content-Length` 请求体、keep-alive 判断、header 大小写不敏感查找
- `Transfer-Encoding: chunked` 的请求暂时返回 `kUnsupportedTransferEncoding`

```cpp
#include "http_request_parser.hpp"

http::HttpRequestParser parser;
std::string_view bytes(buffer, length);
size_t consumed = 0;

switch (parser.Parse(bytes, consumed))
{
case http::ParseStatus::NeedMore:
    // 数据不够，保留 buffer 等下一次
    break;
case http::ParseStatus::Complete:
    // parser.Request() 有效；处理完后 parser.Reset()
    break;
case http::ParseStatus::Error:
    // parser.error() 区分 400 / 413 / 431 / 501
    break;
}
```

`consumed` 是本调用消费的字节数：

- `NeedMore`：可能是 0，也可能是 header 已消费、body 只到一半
- `Complete`：本调用消费了整条消息剩余部分
- `Error`：为 0，连接应关闭

测试：

```bash
cd http
make test
make asan
```

测试覆盖：完整 GET/POST、逐字节喂入、header/body 分包、两条请求拼接、畸形请求、
重复/冲突/非法 `Content-Length`、大小限制、HTTP/1.0 与 1.1 keep-alive。

## 响应 writer

`http_response_writer.hpp` 只负责把状态行、header、body 变成字节，通过 `ByteSink` 输出，
不依赖 TCP：

```cpp
http::HttpResponseWriter writer(
    [conn](const char* data, size_t len) { conn->Send(data, len); },
    request.keep_alive);

writer.WriteHeader(200, "OK", headers,
                   http::HttpResponseWriter::BodyMode::kFixed, body.size());
writer.WriteBody(body.data(), body.size());
writer.Finish();
```

三种 body 模式：

- `kNone`：204 / 304 / HEAD，无 body
- `kFixed`：自动写 `Content-Length`
- `kChunked`：自动写 `Transfer-Encoding: chunked`，用 `WriteChunk()` 发 SSE

用户 header 里的 `Content-Length`、`Transfer-Encoding`、`Connection` 会被忽略，
以 `BodyMode` 和构造函数的 `keep_alive` 为准。`Finish()` 是幂等的。

测试：

```bash
cd http
make response-writer-test
```

（`make writer-test` 是旧的别名，仍然可用。）

## 上游请求 writer

`http_request_writer.hpp` 把 `HttpRequest` 序列化成上游请求字节：

```cpp
std::string wire =
    http::HttpRequestWriter::Build(request, "127.0.0.1:8080", false);
client.Send(wire.data(), wire.size());
```

- 自动写 `Host`、`Content-Length`、`Connection`
- 用户 header 里的 `Host`、`Content-Length`、`Transfer-Encoding`、
  `Connection` 和其他 hop-by-hop header 会被忽略
- 当前固定 `Accept-Encoding: identity`，避免上游 gzip 后无法解析
- 默认 `Connection: close`，一个请求一条连接

测试：

```bash
cd http
make request-writer-test
```

## 上游响应 parser

`http_response_parser.hpp` 支持：

- `Content-Length`
- `Transfer-Encoding: chunked`（包括 chunk extension、trailer）
- 读到连接关闭的 body（配合 `OnEof()`）
- `HEAD` / `1xx` / `204` / `304` 无 body
- body 流式回调（SSE）

```cpp
http::HttpResponseParser parser;
parser.SetBodyCallback([](const char* data, size_t len) {
    // SSE 直接往下游转发
});

size_t consumed = 0;
auto status = parser.Parse(view, consumed);
// NeedMore: 等更多字节
// Complete: parser.Response() 有效
// Error: parser.error()
// 连接关闭时：parser.OnEof()
```

测试：

```bash
cd http
make response-parser-test
```

## API

```c
// 请求：成功返回 header 的字节数；-2 表示数据还不够；-1 表示解析失败
int phr_parse_request(const char *buf, size_t len,
                      const char **method, size_t *method_len,
                      const char **path, size_t *path_len,
                      int *minor_version,
                      struct phr_header *headers, size_t *num_headers,
                      size_t last_len);

// 响应：成功返回 header 的字节数；-2/-1 同上
int phr_parse_response(const char *buf, size_t len,
                       int *minor_version, int *status,
                       const char **msg, size_t *msg_len,
                       struct phr_header *headers, size_t *num_headers,
                       size_t last_len);

// 只解析 header
int phr_parse_headers(...);

// chunked body 解码（原地改写）
ssize_t phr_decode_chunked(struct phr_chunked_decoder *decoder,
                           char *buf, size_t *bufsz);
```

`last_len` 可以传 0，表示每次从头解析；请求量不大时最简单。想减少重复扫描，可以传上一次已经解析过的长度。

## 请求解析

```cpp
const char* method;
const char* path;
size_t method_len, path_len;
int minor_version;
phr_header headers[64];
size_t num_headers = 64;

int ret = phr_parse_request(buf, len,
                            &method, &method_len,
                            &path, &path_len,
                            &minor_version,
                            headers, &num_headers, 0);

if (ret == -2) {
    // 数据不够，等下一次 recv
} else if (ret == -1) {
    // 400 Bad Request
} else {
    // ret 是 header 的字节数
    // body 从 buf + ret 开始
    // 根据 Content-Length 判断 body 是否到齐
}
```

关键点：

- `ret` 只表示 **header 结束的位置**，不包含 body。
- body 长度自己从 `Content-Length` 读；请求是 chunked 的话，用 `phr_decode_chunked`。
- header 的 `name` / `value` 指针指向你传入的 `buf`，**不是独立字符串，也没有 `\0` 结尾**；要保存就按长度拷贝。

## 响应 + chunked

```cpp
int ret = phr_parse_response(buf, len, &minor, &status, &msg, &msg_len,
                             headers, &num_headers, 0);
if (ret > 0) {
    // header 长度 ret；body 从 buf + ret 开始
    // 如果 Transfer-Encoding: chunked：
    std::string body(buf + ret, len - ret);
    phr_chunked_decoder decoder{};
    decoder.consume_trailer = 1;
    size_t body_len = body.size();
    ssize_t left = phr_decode_chunked(&decoder, &body[0], &body_len);
    if (left == -2) {
        // 数据还不够
    } else if (left < 0) {
        // 解析失败
    } else {
        // 解码后的 body 在 body.data()，长度 body_len
        // left 是 chunked 数据之后剩余的字节数（比如下一条响应）
    }
}
```

`decoder.consume_trailer = 1` 很重要：默认 0 时，`0\r\n\r\n` 最后的 `\r\n` 会被当成 leftover 返回 2 字节，容易误判。

## 接进 TcpServer / TcpClient

服务端：

```cpp
void OnMessage(const std::shared_ptr<Connection>& conn, Buffer* in) {
    // 把 in->ReadPositon() / in->AbleReadSpace() 传给 phr_parse_request
    // ret == -2：直接返回，等更多字节
    // ret > 0：解析 header；再根据 Content-Length 等 body
    // 完整一条消息后：in->MoveReadIdx(header_len + body_len)
}
```

客户端：

```cpp
void OnMessage(const std::shared_ptr<Connection>& conn, Buffer* in) {
    // 用 phr_parse_response 解析响应头
    // Content-Length：等 body 到齐
    // chunked：把 body 部分交给 phr_decode_chunked
    // SSE：解码后的 body 直接往下游转发，不要攒整个响应
}
```

## 注意

- pico 是 **stateless** 的：没有连接级 parser 对象；每次调用都从 buffer 头开始解析。半包时返回 `-2`，保留 buffer 下次再来。
- pico **不复制** header；buffer 被覆盖/移动前，把需要的 header 拷贝到自己的 `HttpRequest`。
- pico 只支持 HTTP/1.x；不支持 HTTP/2、TLS、序列化、路由。
- 发请求/响应仍然要自己拼状态行、header、`Content-Length` 或 chunked。
- 例子为了简单，body 不完整时整体返回 `NeedMore`，会重复解析 header；真实实现可以在 header 解析成功后记住 body 还差多少，避免重复解析。
- 上生产前建议把 `headers[64]` 的数量、header 总大小、body 大小都设上限。
