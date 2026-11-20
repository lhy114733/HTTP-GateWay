// 最小 HTTP 服务端：/healthz、/echo 和 /auth。
//
// /auth 会把收到的 Authorization 原样返回，用于验证网关是否把
// 客户端的网关 key 替换成了 provider key。
//
// 编译（在 adapter/ 目录下）：
//   make
//   ./build/http_server_test 8080

#include "../adapter/http_server.hpp"

#include <cstdio>
#include <cstdlib>
#include <string>

static void HandleRequest(const http::HttpRequest &req,
                          http::HttpResponseWriter &writer)
{
    if (req.method == "GET" && req.target == "/healthz")
    {
        const std::string body = "{\"ok\":true}";
        http::HeaderMap headers;
        headers.Add("Content-Type", "application/json");
        writer.WriteHeader(200, "OK", headers,
                           http::HttpResponseWriter::BodyMode::kFixed, body.size());
        writer.WriteBody(body.data(), body.size());
        return;
    }

    if (req.method == "GET" && req.target == "/auth")
    {
        const std::string *authorization = req.headers.Get("authorization");
        const std::string body =
            authorization != nullptr ? *authorization : "none";
        http::HeaderMap headers;
        headers.Add("Content-Type", "text/plain; charset=utf-8");
        writer.WriteHeader(200, "OK", headers,
                           http::HttpResponseWriter::BodyMode::kFixed, body.size());
        writer.WriteBody(body.data(), body.size());
        return;
    }

    if (req.method == "POST" && req.target == "/echo")
    {
        http::HeaderMap headers;
        const std::string *content_type = req.headers.Get("content-type");
        headers.Add("Content-Type",
                    content_type != nullptr ? *content_type : "application/octet-stream");
        writer.WriteHeader(200, "OK", headers,
                           http::HttpResponseWriter::BodyMode::kFixed, req.body.size());
        writer.WriteBody(req.body.data(), req.body.size());
        return;
    }

    const std::string body = "not found\n";
    http::HeaderMap headers;
    headers.Add("Content-Type", "text/plain; charset=utf-8");
    writer.WriteHeader(404, "Not Found", headers,
                       http::HttpResponseWriter::BodyMode::kFixed, body.size());
    writer.WriteBody(body.data(), body.size());
}

int main(int argc, char *argv[])
{
    uint16_t port = 8080;
    int thread_count = 0;
    if (argc > 1) port = static_cast<uint16_t>(std::atoi(argv[1]));
    if (argc > 2) thread_count = std::atoi(argv[2]);
    if (thread_count < 0) thread_count = 0;

    setvbuf(stdout, nullptr, _IONBF, 0);

    HttpServer server(port, thread_count);
    server.SetHandler(HandleRequest);

    std::printf("HTTP server listening on 0.0.0.0:%u, threads=%d\n", port, thread_count);
    server.Start();
    return 0;
}
