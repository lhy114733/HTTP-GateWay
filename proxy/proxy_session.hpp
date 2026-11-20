#pragma once

#include "ports.hpp"

#include "../http/http_message.hpp"

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

// 一次代理请求的事务对象。
//
// 只依赖 proxy::UpstreamClient / proxy::DownstreamSink 和 http 数据模型；
// 不依赖 TcpClient、Eventloop、HttpResponseWriter。
class ProxySession : public std::enable_shared_from_this<ProxySession>
{
public:
    using DoneCallback = std::function<void()>;

    ProxySession(std::unique_ptr<UpstreamClient> upstream,
                 std::shared_ptr<DownstreamSink> downstream,
                 http::HttpRequest request)
        : _request(std::move(request)),
          _upstream(std::move(upstream)),
          _downstream(std::move(downstream))
    {
    }

    void SetDoneCallback(DoneCallback callback)
    {
        _done_callback = std::move(callback);
    }

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

        UpstreamCallbacks callbacks;
        callbacks.on_headers = [weak](const http::HttpResponse &response) {
            if (auto self = weak.lock()) self->OnUpstreamHeaders(response);
        };
        callbacks.on_body = [weak](const char *data, size_t len) {
            if (auto self = weak.lock()) self->OnUpstreamBody(data, len);
        };
        callbacks.on_complete = [weak](const http::HttpResponse &response) {
            if (auto self = weak.lock()) self->OnUpstreamComplete(response);
        };
        callbacks.on_error = [weak](std::string message) {
            if (auto self = weak.lock()) self->OnUpstreamError(std::move(message));
        };
        callbacks.on_closed = [weak]() {
            if (auto self = weak.lock()) self->OnUpstreamClosed();
        };

        if (!_upstream->Start(_request, std::move(callbacks)))
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
            _mode = BodyMode::kNone;
        }
        else if (response.headers.HasToken("transfer-encoding", "chunked"))
        {
            _mode = BodyMode::kChunked;
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
                _mode = BodyMode::kFixed;
            }
            else
            {
                // 上游没有长度，读到关闭；下游用 chunked 边收边发。
                _mode = BodyMode::kChunked;
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

        if (_mode == BodyMode::kChunked)
        {
            _downstream->WriteChunk(data, len);
        }
        else if (_mode == BodyMode::kFixed)
        {
            _downstream->WriteBody(data, len);
        }
    }

    void OnUpstreamComplete(const http::HttpResponse &response)
    {
        if (_done) return;

        if (!_headers_sent) OnUpstreamHeaders(response);
        if (_done) return;

        if (_downstream) _downstream->Finish();
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
                                     BodyMode::kFixed, body.size());
            _downstream->WriteBody(body.data(), body.size());
        }

        if (_downstream) _downstream->Finish();
        FinishProxy();
    }

    void OnUpstreamClosed()
    {
        if (_done) return;
        OnUpstreamError("upstream closed before response");
    }

    void FinishProxy()
    {
        if (_done) return;
        _done = true;
        _downstream.reset();

        auto self = shared_from_this(); // 保证 done_callback 返回前对象还活着
        (void)self;

        auto callback = std::move(_done_callback);
        if (callback) callback();
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

    http::HttpRequest _request;
    std::unique_ptr<UpstreamClient> _upstream;
    std::shared_ptr<DownstreamSink> _downstream;

    BodyMode _mode = BodyMode::kNone;
    size_t _content_length = 0;

    bool _started = false;
    bool _headers_sent = false;
    bool _done = false;

    DoneCallback _done_callback;
};

} // namespace proxy
