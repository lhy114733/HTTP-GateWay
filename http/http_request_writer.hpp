#pragma once

#include "http_message.hpp"

#include <string>
#include <string_view>

// 上游 HTTP 请求序列化器。
//
// 只负责把 HttpRequest 变成字节；不建连接、不调用 TcpClient::Send。
// 默认一个请求一条连接（Connection: close），后续做连接池时再改 keep_alive。
namespace http
{

class HttpRequestWriter
{
public:
    // host 是 Host header，例如 "api.deepseek.com" 或 "127.0.0.1:8080"。
    // 为空时尝试用 request.headers 里的 Host。
    static std::string Build(const HttpRequest &request,
                             std::string_view host,
                             bool keep_alive = false)
    {
        const std::string_view method =
            request.method.empty() ? std::string_view("GET") : std::string_view(request.method);
        const std::string_view target =
            request.target.empty() ? std::string_view("/") : std::string_view(request.target);

        std::string host_value(host);
        if (host_value.empty())
        {
            const std::string *existing = request.headers.Get("host");
            if (existing != nullptr) host_value = *existing;
        }

        std::string out;
        out.reserve(256 + request.body.size());

        out.append(method.data(), method.size());
        out.push_back(' ');
        out.append(target.data(), target.size());
        out += " HTTP/1.1\r\n";

        if (!host_value.empty())
        {
            out += "Host: ";
            out += host_value;
            out += "\r\n";
        }

        for (const Header &header : request.headers.All())
        {
            if (IsSkippedHeader(header.name)) continue;

            out.append(header.name);
            out += ": ";
            out.append(header.value);
            out += "\r\n";
        }

        // 上游响应可能是 gzip；当前响应解析器不解压，先要求 identity。
        out += "Accept-Encoding: identity\r\n";

        out += "Content-Length: ";
        out += std::to_string(request.body.size());
        out += "\r\n";

        out += keep_alive ? "Connection: keep-alive\r\n" : "Connection: close\r\n";
        out += "\r\n";
        out.append(request.body);

        return out;
    }

private:
    static bool IsSkippedHeader(std::string_view name)
    {
        // 这些要么由 writer 重新生成，要么是逐跳 header，不能透传给上游。
        return name == "host" ||
               name == "content-length" ||
               name == "transfer-encoding" ||
               name == "connection" ||
               name == "proxy-connection" ||
               name == "proxy-authorization" ||
               name == "keep-alive" ||
               name == "upgrade" ||
               name == "te" ||
               name == "trailer" ||
               name == "accept-encoding";
    }
};

} // namespace http
