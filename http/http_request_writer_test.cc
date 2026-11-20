// HttpRequestWriter 的纯字符串测试。
//
// 编译：
//   cd http && make request-writer-test

#include "http_request_writer.hpp"

#include <cstdio>
#include <string>

static int g_failed = 0;

static void Check(bool ok, const std::string &name)
{
    std::printf("[%s] %s\n", ok ? " OK " : "FAIL", name.c_str());
    if (!ok) ++g_failed;
}

static void TestBasicPost()
{
    http::HttpRequest req;
    req.method = "POST";
    req.target = "/v1/chat/completions";
    req.headers.Add("Host", "old.example");
    req.headers.Add("Content-Type", "application/json");
    req.headers.Add("Authorization", "Bearer sk-test");
    req.headers.Add("Content-Length", "999");
    req.headers.Add("Connection", "keep-alive");
    req.headers.Add("Accept-Encoding", "gzip");
    req.body = "{\"model\":\"x\"}";

    const std::string wire =
        http::HttpRequestWriter::Build(req, "api.deepseek.com", false);

    const std::string expected =
        "POST /v1/chat/completions HTTP/1.1\r\n"
        "Host: api.deepseek.com\r\n"
        "content-type: application/json\r\n"
        "authorization: Bearer sk-test\r\n"
        "Accept-Encoding: identity\r\n"
        "Content-Length: 13\r\n"
        "Connection: close\r\n"
        "\r\n"
        "{\"model\":\"x\"}";

    Check(wire == expected, "request writer: basic POST");
}

static void TestKeepAliveAndHostFallback()
{
    http::HttpRequest req;
    req.method = "GET";
    req.target = "/healthz";
    req.headers.Add("Host", "gateway.local");
    req.body = "";

    const std::string wire =
        http::HttpRequestWriter::Build(req, "", true);

    const std::string expected =
        "GET /healthz HTTP/1.1\r\n"
        "Host: gateway.local\r\n"
        "Accept-Encoding: identity\r\n"
        "Content-Length: 0\r\n"
        "Connection: keep-alive\r\n"
        "\r\n";

    Check(wire == expected, "request writer: host fallback + keep-alive");
}

static void TestDefaultsAndHopByHop()
{
    http::HttpRequest req;
    req.headers.Add("Proxy-Connection", "keep-alive");
    req.headers.Add("Keep-Alive", "timeout=5");
    req.headers.Add("TE", "trailers");
    req.headers.Add("Upgrade", "h2c");
    req.headers.Add("Trailer", "X-Test");
    req.headers.Add("Proxy-Authorization", "Basic abc");
    req.headers.Add("X-Real", "1");

    const std::string wire =
        http::HttpRequestWriter::Build(req, "example.com", false);

    const std::string expected =
        "GET / HTTP/1.1\r\n"
        "Host: example.com\r\n"
        "x-real: 1\r\n"
        "Accept-Encoding: identity\r\n"
        "Content-Length: 0\r\n"
        "Connection: close\r\n"
        "\r\n";

    Check(wire == expected, "request writer: defaults + hop-by-hop");
}

int main()
{
    TestBasicPost();
    TestKeepAliveAndHostFallback();
    TestDefaultsAndHopByHop();

    std::printf("\n%s (%d failed)\n", g_failed == 0 ? "PASS" : "FAIL", g_failed);
    return g_failed == 0 ? 0 : 1;
}
