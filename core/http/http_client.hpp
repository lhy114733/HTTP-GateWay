#pragma once

#include "http_request_writer.hpp"
#include "http_response_parser.hpp"
#include "../net/TcpClient.hpp"

#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

// HTTP 上游客户端：TcpClient + 请求序列化 + 响应解析。
//
// 当前是“一条连接一个请求”的简单版本：
//   Send(request) -> Connect -> 发送 -> 收完响应 -> Disconnect
//
// host/host_port 决定 HTTP Host 头；connect_ip/connect_port 决定 TCP 连接。
// 两者分开是为了走 stunnel：host=api.deepseek.com, host_port=443,
// connect_ip=127.0.0.1, connect_port=18443。
// 域名可以在启动时用 HostResolver 解析成 connect_ip，运行期不再查 DNS。
// 也没有 TLS，TLS 由 stunnel 负责，这条连接本身是明文 HTTP。
class HttpClient
{
private:
    enum class State
    {
        kIdle,
        kConnecting,
        kSending,
        kReceiving,
        kDone
    };
    using HeadersCallback = std::function<void(const http::HttpResponse &)>;
    using ResponseCallback = std::function<void(const http::HttpResponse &)>;
    using BodyCallback = std::function<void(const char *, size_t)>;
    using ErrorCallback = std::function<void(const std::string &)>;
    using ClosedCallback = std::function<void()>;

    std::string _host;
    uint16_t _host_port;
    std::string _host_header;
    TcpClient _client;
    http::HttpRequest _request;
    http::HttpResponseParser _response_parser;
    State _state;

    HeadersCallback _headers_callback;
    ResponseCallback _response_callback;
    BodyCallback _body_callback;
    ErrorCallback _error_callback;
    ClosedCallback _closed_callback;
    bool _closed_notified = false;
public:
    // 完整形式：Host 头和实际连接地址可以完全不同。
    HttpClient(Eventloop *loop, std::string host, uint16_t host_port,
                      std::string connect_ip, uint16_t connect_port)
        : _host(std::move(host)), _host_port(host_port), _host_header(_host),
          _client(loop, connect_port, connect_ip.empty() ? _host : connect_ip),
          _state(State::kIdle)
    {
        // 80/443 是默认端口，Host 头里不写；sidecar 的本地端口不参与 Host。
        // Host: <host>[:<port>]默认没port，走默认的80和443端口，有则加上
        if (_host_port != 80 && _host_port != 443) 
        {
            _host_header += ":";
            _host_header += std::to_string(_host_port);
        }
        _client.SetConnectedCallback(std::bind(&HttpClient::HandleConnected, this, std::placeholders::_1));
        _client.SetMessageCallback(std::bind(&HttpClient::HandleMessage, this, std::placeholders::_1, std::placeholders::_2));
        _client.SetClosedCallback(std::bind(&HttpClient::HandleClosed, this, std::placeholders::_1));
        _client.SetConnectErrorCallback(std::bind(&HttpClient::HandleConnectError, this, std::placeholders::_1));
    }

    // 设置响应头到达回调；在 body 之前触发，用于先写下游响应头。
    void SetHeadersCallback(const HeadersCallback &callback)
    {
        _headers_callback = callback;

        if (_headers_callback)
        {
            _response_parser.SetHeadersCallback([this](const http::HttpResponse &response) {
                if (_headers_callback) _headers_callback(response);
            });
        }
        else
        {
            _response_parser.SetHeadersCallback({});
        }
    }
    // 设置完整响应结束回调；收到完整响应或 EOF 结束时触发。
    void SetResponseCallback(const ResponseCallback &callback) { _response_callback = callback; }

    // 设置流式 body 回调；设置后 body 不再累加到 Response::body。
    // 整条 HTTP 响应解析完成。 代理调用下游 writer 的 Finish()，结束这次响应
    void SetBodyCallback(const BodyCallback &callback)
    {
        _body_callback = callback;

        if (_body_callback)
        {
            _response_parser.SetBodyCallback([this](const char *data, size_t len) {
                if (_body_callback) _body_callback(data, len);
            });
        }
        else
        {
            // 没有流式回调时，让 parser 把 body 累加到 Response().body。
            _response_parser.SetBodyCallback({});
        }
    }

    // 设置服务端错误回调，例如连接失败、解析失败、响应被截断。
    // 建连、发送或响应解析出了错
    void SetErrorCallback(const ErrorCallback &callback) { _error_callback = callback; }

    // 连接真正关闭后调用；HttpClient 至少要活到这个回调。
    void SetClosedCallback(const ClosedCallback &callback) { _closed_callback = callback; }

    // 异步：连接建立后自动发送；收到完整响应后回调 ResponseCallback。
    bool Send(const http::HttpRequest &request, int timeout_sec = 3)
    {
        if (_state != State::kIdle) return false;

        _request = request;
        _response_parser.Reset();
        _response_parser.SetExpectNoBody(request.method == "HEAD");
        _state = State::kConnecting;

        _client.Connect(timeout_sec);
        return true;
    }

    // 优雅关闭：把已排队的输出发完后断开。
    void Close()
    {
        if (_state == State::kDone) return;
        _state = State::kDone;
        _client.Disconnect();
    }

    // 立即中止上游，不等待响应。
    void Cancel()
    {
        if (_state == State::kDone) return;
        _state = State::kDone;
        _client.Abort();
    }

private:
    // 连接建立后序列化请求并发送给上游。
    void HandleConnected(const std::shared_ptr<Connection> &)
    {
        if (_state != State::kConnecting) return;

        _state = State::kSending;

        const std::string wire = http::HttpRequestWriter::Build(_request, _host_header, false);

        if (!_client.Send(wire.data(), wire.size()))
        {
            Fail("send failed", true);
            return;
        }

        _state = State::kReceiving;
    }

    // 增量解析上游响应；收到完整响应后结束本次请求。
    void HandleMessage(const std::shared_ptr<Connection> &, Buffer *in)
    {
        if (_state != State::kReceiving) return;

        std::string_view view(in->ReadPositon(), static_cast<size_t>(in->AbleReadSpace()));
        size_t used = 0;

        const http::ParseStatus status = _response_parser.Parse(view, used);
        in->MoveReadIdx(used);

        if (status == http::ParseStatus::NeedMore) return;

        if (status == http::ParseStatus::Error)
        {
            Fail(ErrorText(_response_parser.error()), true);
            return;
        }

        FinishResponse(false);
    }

    // 底层连接关闭时，用 OnEof 判断响应是正常结束还是被截断。
    void HandleClosed(const std::shared_ptr<Connection> &)
    {
        if (_state == State::kReceiving)
        {
            const http::ParseStatus status = _response_parser.OnEof();

            if (status == http::ParseStatus::Complete)
            {
                FinishResponse(true);
            }
            else if (status == http::ParseStatus::Error)
            {
                Fail(ErrorText(_response_parser.error()), false);
            }
        }
        else if (_state == State::kConnecting || _state == State::kSending)
        {
            Fail("connection closed before response", false);
        }

        // 通知已关闭
        NotifyClosed();
    }

    // 连接失败时转成错误回调，并通知连接已关闭。
    void HandleConnectError(int err)
    {
        Fail(std::string("connect failed: ") + std::strerror(err), false);
        NotifyClosed();
    }

    // 成功完成：标记结束、关闭连接，并回调完整响应。
    void FinishResponse(bool from_eof)
    {
        if (_state == State::kDone) return;
        _state = State::kDone;

        // 先关闭连接，保证 Connection::Release 的 queued task 排在
        // 上层“销毁 ProxySession”之前；否则 closed 回调
        // 会打到已释放的 HttpClient 上。
        if (!from_eof) _client.Disconnect();

        if (_response_callback) _response_callback(_response_parser.Response());
    }

    // 失败路径：标记结束、按需关闭连接，并回调错误信息。
    void Fail(const std::string &message, bool close_connection)
    {
        if (_state == State::kDone) return;
        _state = State::kDone;

        if (close_connection) _client.Disconnect();

        if (_error_callback) _error_callback(message);
    }

    // 调用连接关闭回调
    void NotifyClosed()
    {
        if (_closed_notified) return;
        _closed_notified = true;
        if (_closed_callback) _closed_callback();
    }

    // 把响应解析错误转换成可读的错误文本。
    static const char *ErrorText(http::ResponseParseError error)
    {
        switch (error)
        {
        case http::ResponseParseError::kMalformed:
            return "malformed response";
        case http::ResponseParseError::kHeaderTooLarge:
            return "response header too large";
        case http::ResponseParseError::kBodyTooLarge:
            return "response body too large";
        case http::ResponseParseError::kBadContentLength:
            return "bad content-length";
        case http::ResponseParseError::kUnsupportedTransferEncoding:
            return "unsupported transfer-encoding";
        case http::ResponseParseError::kChunkedError:
            return "malformed chunked body";
        case http::ResponseParseError::kTruncated:
            return "truncated response";
        case http::ResponseParseError::kNone:
            return "response error";
        }
        return "response error";
    }
};
