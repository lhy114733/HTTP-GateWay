#pragma once

#include "http_message.hpp"

#include <charconv>
#include <cstddef>
#include <functional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

// 服务端 HTTP 响应序列化器。
//
// 它只负责“把响应变成字节”，通过 ByteSink 输出，不依赖网络层。
// 适配层把 sink 接到 conn->Send() 上即可。
//
// 用法：
//   http::HttpResponseWriter writer(
//       [conn](const char* data, size_t len) { conn->Send(data, len); },
//       request.keep_alive);
//
//   writer.WriteHeader(200, "OK", headers,
//                      http::HttpResponseWriter::BodyMode::kFixed, body.size());
//   writer.WriteBody(body.data(), body.size());
//   writer.Finish();
namespace http
{

class HttpResponseWriter
{
public:
    using ByteSink = std::function<void(const char*, size_t)>;
    using FinishCallback = std::function<void()>;

    enum class BodyMode
    {
        kNone,   // 204 / 304 / HEAD
        kFixed,  // Content-Length
        kChunked // SSE / 长度未知
    };

    HttpResponseWriter(ByteSink sink, bool keep_alive)
        : _sink(std::move(sink)), _keep_alive(keep_alive)
    {
    }

    // 响应真正结束时调用；只调用一次，异步代理用它通知上层恢复。
    void SetFinishCallback(FinishCallback callback)
    {
        _finish_callback = std::move(callback);
    }

    // 只允许调用一次；重复调用会被忽略。
    // kFixed 时必须给出 content_length，writer 会自动写 Content-Length。
    // kChunked 时自动写 Transfer-Encoding: chunked。
    // 用户 headers 里的 Content-Length / Transfer-Encoding / Connection 会被忽略，
    // 以 writer 的 BodyMode 和 keep_alive 为准。
    void WriteHeader(int status,
                     std::string_view reason,
                     const HeaderMap &headers,
                     BodyMode mode,
                     size_t content_length = 0)
    {
        if (_header_sent) return;
        if (_finished) return;
        _header_sent = true;

        const std::string_view text = reason.empty() ? StatusText(status) : reason;

        std::string out;
        out.reserve(256);

        out += "HTTP/1.1 ";
        out += std::to_string(status);
        out.push_back(' ');
        out.append(text.data(), text.size());
        out += "\r\n";

        for (const Header &header : headers.All())
        {
            if (IsWriterManagedHeader(header.name)) continue;

            out.append(header.name);
            out += ": ";
            out.append(header.value);
            out += "\r\n";
        }

        if (mode == BodyMode::kFixed)
        {
            out += "Content-Length: ";
            out += std::to_string(content_length);
            out += "\r\n";
        }
        else if (mode == BodyMode::kChunked)
        {
            out += "Transfer-Encoding: chunked\r\n";
        }

        out += _keep_alive ? "Connection: keep-alive\r\n" : "Connection: close\r\n";
        out += "\r\n";

        _mode = mode;
        Emit(out.data(), out.size());
    }

    // kFixed 模式下的 body。
    void WriteBody(const char *data, size_t len)
    {
        if (!_header_sent || _finished || _mode != BodyMode::kFixed) return;
        if (len > 0) Emit(data, len);
    }

    // kChunked 模式下的一个 chunk。len == 0 时什么都不做，结束用 Finish()。
    void WriteChunk(const char *data, size_t len)
    {
        if (!_header_sent || _finished || _mode != BodyMode::kChunked) return;
        if (len == 0) return;

        char size_buf[32];
        const auto result = std::to_chars(size_buf, size_buf + sizeof(size_buf), len, 16);
        if (result.ec != std::errc()) return;

        std::string prefix(size_buf, result.ptr);
        prefix += "\r\n";
        Emit(prefix.data(), prefix.size());
        Emit(data, len);
        Emit("\r\n", 2);
    }

    // 幂等；chunked 模式会补上 0\r\n\r\n。
    void Finish()
    {
        if (_finished) return;
        _finished = true;

        if (_header_sent && _mode == BodyMode::kChunked)
        {
            Emit("0\r\n\r\n", 5);
        }

        if (_finish_callback)
        {
            auto callback = std::move(_finish_callback);
            callback();
        }
    }

    bool finished() const { return _finished; }

    bool header_sent() const { return _header_sent; }

    static std::string_view StatusText(int status)
    {
        switch (status)
        {
        case 200: return "OK";
        case 201: return "Created";
        case 202: return "Accepted";
        case 204: return "No Content";
        case 301: return "Moved Permanently";
        case 302: return "Found";
        case 304: return "Not Modified";
        case 400: return "Bad Request";
        case 401: return "Unauthorized";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 408: return "Request Timeout";
        case 413: return "Payload Too Large";
        case 414: return "URI Too Long";
        case 415: return "Unsupported Media Type";
        case 429: return "Too Many Requests";
        case 431: return "Request Header Fields Too Large";
        case 500: return "Internal Server Error";
        case 501: return "Not Implemented";
        case 502: return "Bad Gateway";
        case 503: return "Service Unavailable";
        case 504: return "Gateway Timeout";
        default: return "Unknown";
        }
    }

private:
    static bool IsWriterManagedHeader(std::string_view name)
    {
        return name == "content-length" ||
               name == "transfer-encoding" ||
               name == "connection";
    }

    void Emit(const char *data, size_t len)
    {
        if (_sink && len > 0) _sink(data, len);
    }

    ByteSink _sink;
    FinishCallback _finish_callback;
    bool _keep_alive;
    bool _header_sent = false;
    bool _finished = false;
    BodyMode _mode = BodyMode::kNone;
};

} // namespace http
