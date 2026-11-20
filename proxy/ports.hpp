#pragma once

#include "../http/http_message.hpp"

#include <cstddef>
#include <functional>
#include <string>
#include <string_view>

namespace proxy
{

enum class BodyMode
{
    kNone,
    kFixed,
    kChunked
};

struct UpstreamCallbacks
{
    std::function<void(const http::HttpResponse &)> on_headers;
    std::function<void(const char *, size_t)> on_body;
    std::function<void(const http::HttpResponse &)> on_complete;
    std::function<void(std::string)> on_error;
    std::function<void()> on_closed;
};

// 上游客户端端口：只负责一次请求的生命周期，不知道 proxy 事务细节。
class UpstreamClient
{
public:
    virtual ~UpstreamClient() = default;

    virtual bool Start(const http::HttpRequest &request,
                       UpstreamCallbacks callbacks) = 0;

    virtual void Cancel() = 0;
};

// 下游响应端口：ProxySession 只通过这个接口写响应。
class DownstreamSink
{
public:
    virtual ~DownstreamSink() = default;

    virtual void WriteHeader(int status,
                             std::string_view reason,
                             const http::HeaderMap &headers,
                             BodyMode mode,
                             size_t content_length) = 0;

    virtual void WriteBody(const char *data, size_t len) = 0;
    virtual void WriteChunk(const char *data, size_t len) = 0;
    virtual void Finish() = 0;
};

class TaskRunner
{
public:
    virtual ~TaskRunner() = default;
    virtual void Post(std::function<void()> task) = 0;
};

} // namespace proxy
