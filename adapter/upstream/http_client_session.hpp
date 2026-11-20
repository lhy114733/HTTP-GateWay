#pragma once

#include "../../core/http/http_request_writer.hpp"
#include "../../core/http/http_response_parser.hpp"
#include "../../core/net/TcpClient.hpp"

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
class HttpClientSession
{
public:
    using HeadersCallback = std::function<void(const http::HttpResponse &)>;
    using ResponseCallback = std::function<void(const http::HttpResponse &)>;
    using BodyCallback = std::function<void(const char *, size_t)>;
    using ErrorCallback = std::function<void(const std::string &)>;
    using ClosedCallback = std::function<void()>;

    // 完整形式：Host 头和实际连接地址可以完全不同。
    HttpClientSession(Eventloop *loop, std::string host, uint16_t host_port,
                      std::string connect_ip, uint16_t connect_port)
        : _host(std::move(host)), _host_port(host_port), _host_header(_host),
          _client(loop, connect_port, connect_ip.empty() ? _host : connect_ip),
          _state(State::kIdle)
    {
        // 80/443 是默认端口，Host 头里不写；sidecar 的本地端口不参与 Host。
        if (_host_port != 80 && _host_port != 443)
        {
            _host_header += ":";
            _host_header += std::to_string(_host_port);
        }

        _client.SetConnectedCallback(
            std::bind(&HttpClientSession::HandleConnected, this, std::placeholders::_1));
        _client.SetMessageCallback(
            std::bind(&HttpClientSession::HandleMessage, this,
                      std::placeholders::_1, std::placeholders::_2));
        _client.SetClosedCallback(
            std::bind(&HttpClientSession::HandleClosed, this, std::placeholders::_1));
        _client.SetConnectErrorCallback(
            std::bind(&HttpClientSession::HandleConnectError, this, std::placeholders::_1));
    }

    // 直连：连接地址和 Host 用同一组 host/port。
    HttpClientSession(Eventloop *loop, std::string host, uint16_t port)
        : HttpClientSession(loop, std::move(host), port, std::string{}, port)
    {
    }

    // 直连但连接地址单独指定（host 是域名，connect_ip 是解析好的 IP）。
    HttpClientSession(Eventloop *loop, std::string host, uint16_t port,
                      std::string connect_ip)
        : HttpClientSession(loop, std::move(host), port,
                            std::move(connect_ip), port)
    {
    }

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

    void SetResponseCallback(const ResponseCallback &callback)
    {
        _response_callback = callback;
    }

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

    void SetErrorCallback(const ErrorCallback &callback)
    {
        _error_callback = callback;
    }

    // 连接真正关闭后调用；HttpClientSession 至少要活到这个回调。
    void SetClosedCallback(const ClosedCallback &callback)
    {
        _closed_callback = callback;
    }

    // 转发上游时使用的认证头。发送前会先删掉客户端带来的 Authorization，
    // 防止网关 key 被透传给 provider；value 为空时只删除、不注入。
    void SetUpstreamAuth(std::string header, std::string value)
    {
        if (header.empty()) header = "Authorization";
        _upstream_auth_header = std::move(header);
        _upstream_auth_value = std::move(value);
    }

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
    enum class State
    {
        kIdle,
        kConnecting,
        kSending,
        kReceiving,
        kDone
    };

    void HandleConnected(const std::shared_ptr<Connection> &)
    {
        if (_state != State::kConnecting) return;

        _state = State::kSending;

        // 不直接改 _request：ProxySession 还可能持有原始请求。
        http::HttpRequest upstream_request = _request;
        upstream_request.headers.Remove("authorization");
        upstream_request.headers.Remove("proxy-authorization");
        if (!_upstream_auth_value.empty())
        {
            upstream_request.headers.Set(_upstream_auth_header,
                                         _upstream_auth_value);
        }

        const std::string wire =
            http::HttpRequestWriter::Build(upstream_request, _host_header, false);

        if (!_client.Send(wire.data(), wire.size()))
        {
            Fail("send failed", true);
            return;
        }

        _state = State::kReceiving;
    }

    void HandleMessage(const std::shared_ptr<Connection> &, Buffer *in)
    {
        if (_state != State::kReceiving) return;

        std::string_view view(in->ReadPositon(),
                              static_cast<size_t>(in->AbleReadSpace()));
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

        NotifyClosed();
    }

    void HandleConnectError(int err)
    {
        Fail(std::string("connect failed: ") + std::strerror(err), false);
        NotifyClosed();
    }

    void FinishResponse(bool from_eof)
    {
        if (_state == State::kDone) return;
        _state = State::kDone;

        // 先关闭连接，保证 Connection::Release 的 queued task 排在
        // 上层“销毁 ProxySession/HttpClientUpstream”之前；否则 closed 回调
        // 会打到已释放的 HttpClientSession 上。
        if (!from_eof) _client.Disconnect();

        if (_response_callback) _response_callback(_response_parser.Response());
    }

    void Fail(const std::string &message, bool close_connection)
    {
        if (_state == State::kDone) return;
        _state = State::kDone;

        if (close_connection) _client.Disconnect();

        if (_error_callback) _error_callback(message);
    }

    void NotifyClosed()
    {
        if (_closed_notified) return;
        _closed_notified = true;
        if (_closed_callback) _closed_callback();
    }

    static const char *ErrorText(http::HttpResponseParser::Error error)
    {
        switch (error)
        {
        case http::HttpResponseParser::Error::kMalformed:
            return "malformed response";
        case http::HttpResponseParser::Error::kHeaderTooLarge:
            return "response header too large";
        case http::HttpResponseParser::Error::kBodyTooLarge:
            return "response body too large";
        case http::HttpResponseParser::Error::kBadContentLength:
            return "bad content-length";
        case http::HttpResponseParser::Error::kUnsupportedTransferEncoding:
            return "unsupported transfer-encoding";
        case http::HttpResponseParser::Error::kChunkedError:
            return "malformed chunked body";
        case http::HttpResponseParser::Error::kTruncated:
            return "truncated response";
        case http::HttpResponseParser::Error::kNone:
            return "response error";
        }
        return "response error";
    }

    std::string _host;
    uint16_t _host_port;
    std::string _host_header;
    std::string _upstream_auth_header = "Authorization";
    std::string _upstream_auth_value;
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
};
