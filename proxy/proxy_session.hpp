#pragma once

#include "../core/http/http_client.hpp"
#include "../core/http/http_message.hpp"
#include "../core/http/http_response_writer.hpp"

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace proxy
{

// 一次 HTTP 代理请求的事务对象。
//
// 这里直接依赖 HttpClient 和 HttpResponseWriter。
// 不再有 UpstreamClient / DownstreamSink 端口和对应的 adapter 包装。
class ProxySession : public std::enable_shared_from_this<ProxySession>
{
public:
    using DoneCallback = std::function<void()>;
    using WriterMode = http::HttpResponseWriter::BodyMode;

    // ProxySession需要一个给下游发数据的东西，就是HttpResponseWriter
    ProxySession(std::unique_ptr<HttpClient> upstream,
                 std::shared_ptr<http::HttpResponseWriter> downstream,
                 http::HttpRequest request,
                 const std::shared_ptr<Connection> &downstream_connection = {})
        : _request(std::move(request)),
          _upstream(std::move(upstream)),
          _downstream(std::move(downstream)),
          _downstream_connection(downstream_connection)
    {
    }

    void SetDoneCallback(DoneCallback callback)
    {
        _done_callback = std::move(callback);
    }

    // 把上游客户端的五类事件直接接到当前事务上。
    bool Start()
    {
        if (_started) return false;
        _started = true;

        if (!_upstream || !_downstream)
        {
            OnUpstreamError("proxy is not configured");
            return false;
        }

        std::weak_ptr<ProxySession> weak = shared_from_this();

        _upstream->SetHeadersCallback([weak](const http::HttpResponse &response) {
            if (auto self = weak.lock()) self->OnUpstreamHeaders(response);
        });
        _upstream->SetBodyCallback([weak](const char *data, size_t len) {
            if (auto self = weak.lock()) self->OnUpstreamBody(data, len);
        });
        _upstream->SetResponseCallback([weak](const http::HttpResponse &response) {
            if (auto self = weak.lock()) self->OnUpstreamComplete(response);
        });
        _upstream->SetErrorCallback([weak](const std::string &message) {
            if (auto self = weak.lock()) self->OnUpstreamError(message);
        });
        _upstream->SetClosedCallback([weak]() {
            if (auto self = weak.lock()) self->OnUpstreamClosed();
        });

        if (auto conn = _downstream_connection.lock())
        {
            conn->SetOutputWatermarks(kOutputLow, kOutputHigh, kOutputLimit, [weak](bool paused) {
                if (auto self = weak.lock(); self && !self->_done)
                    self->_upstream->SetReadPaused(paused);
            });
        }

        if (!_upstream->Send(_request))
        {
            OnUpstreamError("failed to start upstream request");
            return false;
        }

        return true;
    }

    void Cancel()
    {
        if (_done) return;
        _done = true;

        if (_upstream) _upstream->Cancel();
        _downstream.reset();

        auto self = shared_from_this(); // 保证 done_callback 返回前对象还活着
        (void)self;

        auto callback = std::move(_done_callback);
        if (callback) callback();
    }

private:
    void OnUpstreamHeaders(const http::HttpResponse &response)
    {
        if (_done || _headers_sent || !_downstream) return;

        if (IsNoBodyStatus(response.status) || _request.method == "HEAD")
        {
            _mode = WriterMode::kNone;
        }
        else if (response.headers.HasToken("transfer-encoding", "chunked"))
        {
            _mode = WriterMode::kChunked;
        }
        else
        {
            const std::string *length = response.headers.Get("content-length");
            if (length != nullptr)
            {
                size_t value = 0;
                if (!ParseContentLength(*length, value))
                {
                    OnUpstreamError("bad upstream content-length");
                    return;
                }
                _content_length = value;
                _mode = WriterMode::kFixed;
            }
            else
            {
                // 上游没有长度，读到关闭；下游用 chunked 边收边发。
                _mode = WriterMode::kChunked;
            }
        }

        http::HeaderMap headers;
        for (const http::Header &header : response.headers.All())
        {
            if (IsHopByHopHeader(header.name)) continue;
            headers.Add(header.name, header.value);
        }

        _headers_sent = true;
        _downstream->WriteHeader(response.status, response.reason, headers,
                                 _mode, _content_length);
    }

    void OnUpstreamBody(const char *data, size_t len)
    {
        if (_done || !_headers_sent || !_downstream) return;

        if (_mode == WriterMode::kChunked)
        {
            _downstream->WriteChunk(data, len);
        }
        else if (_mode == WriterMode::kFixed)
        {
            _downstream->WriteBody(data, len);
        }
    }

    void OnUpstreamComplete(const http::HttpResponse &response)
    {
        if (_done) return;

        if (!_headers_sent) OnUpstreamHeaders(response);
        if (_done) return;

        FinishDownstream();
        FinishProxy();
    }

    void OnUpstreamError(const std::string &message)
    {
        if (_done) return;

        if (!_headers_sent && _downstream)
        {
            std::string body = "bad gateway: ";
            body += message;
            body += "\n";

            http::HeaderMap headers;
            headers.Add("Content-Type", "text/plain; charset=utf-8");

            _downstream->WriteHeader(502, "Bad Gateway", headers,
                                     WriterMode::kFixed, body.size());
            _downstream->WriteBody(body.data(), body.size());
        }

        FinishDownstream();
        FinishProxy();
    }

    void OnUpstreamClosed()
    {
        if (_done) return;
        OnUpstreamError("upstream closed before response");
    }

    void FinishDownstream()
    {
        if (auto conn = _downstream_connection.lock(); conn && conn->OutputAborted()) return;
        if (_downstream) _downstream->Finish();
    }

    void FinishProxy()
    {
        if (_done) return;
        _done = true;
        _downstream.reset();

        auto self = shared_from_this(); // 保证 done_callback 返回前对象还活着
        (void)self;

        if (_done_callback) _done_callback();
    }

    static bool IsNoBodyStatus(int status)
    {
        return (status >= 100 && status < 200) || status == 204 || status == 304;
    }

    static bool IsHopByHopHeader(std::string_view name)
    {
        return name == "connection" ||
               name == "keep-alive" ||
               name == "proxy-connection" ||
               name == "transfer-encoding" ||
               name == "te" ||
               name == "trailer" ||
               name == "upgrade" ||
               name == "content-length";
    }

    static bool ParseContentLength(std::string_view value, size_t &out)
    {
        value = http::detail::TrimOws(value);
        if (value.empty()) return false;

        uint64_t parsed = 0;
        const char *begin = value.data();
        const char *end = begin + value.size();
        const auto result = std::from_chars(begin, end, parsed);

        if (result.ec != std::errc() || result.ptr != end) return false;
        if (parsed > static_cast<uint64_t>(std::numeric_limits<size_t>::max()))
        {
            return false;
        }

        out = static_cast<size_t>(parsed);
        return true;
    }

    static constexpr size_t kOutputLow = 256 * 1024;
    static constexpr size_t kOutputHigh = 512 * 1024;
    static constexpr size_t kOutputLimit = 1024 * 1024;

    http::HttpRequest _request;
    std::unique_ptr<HttpClient> _upstream;
    std::shared_ptr<http::HttpResponseWriter> _downstream;
    std::weak_ptr<Connection> _downstream_connection;

    WriterMode _mode = WriterMode::kNone;
    size_t _content_length = 0;

    bool _started = false;
    bool _headers_sent = false;
    bool _done = false;

    DoneCallback _done_callback;
};

} // namespace proxy
