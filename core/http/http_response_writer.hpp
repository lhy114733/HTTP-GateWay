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

    // HttpResponseWriter构造时已经捕获了一个conn(指针)，
    // _sink(char*,size_t) -> Emit(char*,size_t)，
    // 每次调用Emit()都相当于向这个conn连接Send()一次！！！
    // 通过WriterHeader()和WriterBody调Emit(),
    // 所以必须先WriterHeader()再WriterBody()，不然Send()顺序就反了
    // 这种回调写法算一种思路吧，其实挺好的，但是不是很容易理解，
    // json_cpp的直接返回一个string更好理解还是。
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
        if (_header_sent) return; // 已发送header
        if (_finished) return; // 已发送完，都直接返回
        _header_sent = true; //说明已发送

        // 有reason就直接用提供的reason，没有就StatusText(status)解析状态码的reason
        const std::string_view text = reason.empty() ? StatusText(status) : reason;

        std::string out;
        out.reserve(256);

        // 写响应行
        out += "HTTP/1.1 ";
        out += std::to_string(status);
        out.push_back(' ');
        out.append(text.data(), text.size());
        out += "\r\n";

        // 写报头，跳过IsWriterManagedHeader的Header
        for (const Header &header : headers.All())
        {
            if (IsWriterManagedHeader(header.name)) continue;

            out.append(header.name);
            out += ": ";
            out.append(header.value);
            out += "\r\n";
        }

        // 写body相关Header：Content-Length || Transfer-Encoding
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

        // 写是否长连接Header：Connection
        out += _keep_alive ? "Connection: keep-alive\r\n" : "Connection: close\r\n";
        out += "\r\n";

        _mode = mode;
        Emit(out.data(), out.size()); // 发送给客户端
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

        // kChunked模式下补上 0\r\n\r\n
        if (_header_sent && _mode == BodyMode::kChunked)
        {
            Emit("0\r\n\r\n", 5);
        }

        if (_finish_callback)
        {
            _finish_callback();
        }
    }

    // 是否发完头，是否发完body
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

    ByteSink _sink; // 理解发出回调的意义，参数是data和len，发给谁怎么发由设置回调函数的人决定，
    // 类似于一个约定，调用这个回调就是为了发送，实现了发送方式的解耦
    FinishCallback _finish_callback;
    bool _keep_alive;
    bool _header_sent = false;
    bool _finished = false;
    BodyMode _mode = BodyMode::kNone;
};

} // namespace http
