// 纯 HTTP 请求 codec 的单元测试：不依赖 socket、Buffer、TcpServer。
//
// 编译：
//   make test
//   make asan

#include "http_request_parser.hpp"

#include <cstdio>
#include <string>

static int g_failed = 0;

static void Check(bool ok, const std::string &name)
{
    std::printf("[%s] %s\n", ok ? " OK " : "FAIL", name.c_str());
    if (!ok) ++g_failed;
}

// 反复解析，直到 Complete / Error / 数据不够。
// input 会被原地消费掉 parser 已经处理的部分。
static http::ParseStatus Feed(http::HttpRequestParser &parser,
                              std::string &input,
                              size_t &total_consumed)
{
    total_consumed = 0;

    for (;;)
    {
        size_t consumed = 0;
        const http::ParseStatus status = parser.Parse(input, consumed);

        if (consumed > input.size())
        {
            Check(false, "parser consumed more than input");
            return http::ParseStatus::Error;
        }

        total_consumed += consumed;
        input.erase(0, consumed);

        if (status != http::ParseStatus::NeedMore) return status;
        if (consumed == 0) return status;
    }
}

static std::string MakeJsonRequest()
{
    const std::string body = "{\"model\":\"x\"}";
    return "POST /v1/chat/completions HTTP/1.1\r\n"
           "Host: gateway.local\r\n"
           "Content-Type: application/json\r\n"
           "Content-Length: " +
           std::to_string(body.size()) + "\r\n"
           "\r\n" +
           body;
}

static void TestSimpleGet()
{
    http::HttpRequestParser parser;
    std::string input = "GET /healthz HTTP/1.1\r\nHost: gateway.local\r\n\r\n";
    const size_t expected = input.size();
    size_t used = 0;

    const http::ParseStatus status = Feed(parser, input, used);

    Check(status == http::ParseStatus::Complete, "GET: complete");
    Check(used == expected && input.empty(), "GET: consumed all bytes");
    Check(parser.Request().method == "GET", "GET: method");
    Check(parser.Request().target == "/healthz", "GET: target");
    Check(parser.Request().minor_version == 1, "GET: HTTP/1.1");
    Check(parser.Request().VersionString() == "HTTP/1.1", "GET: version string");
    Check(parser.Request().body.empty(), "GET: empty body");
    Check(parser.Request().keep_alive, "GET: HTTP/1.1 keep-alive default");

    const std::string *host = parser.Request().headers.Get("host");
    Check(host != nullptr && *host == "gateway.local", "GET: host header");
}

static void TestPostWithBody()
{
    http::HttpRequestParser parser;
    const std::string raw = MakeJsonRequest();
    std::string input = raw;
    const size_t expected = input.size();
    size_t used = 0;

    const http::ParseStatus status = Feed(parser, input, used);

    Check(status == http::ParseStatus::Complete, "POST: complete");
    Check(used == expected && input.empty(), "POST: consumed all bytes");
    Check(parser.Request().method == "POST", "POST: method");
    Check(parser.Request().target == "/v1/chat/completions", "POST: target");
    Check(parser.Request().body == "{\"model\":\"x\"}", "POST: body");

    const std::string *length = parser.Request().headers.Get("content-length");
    Check(length != nullptr && *length == "13", "POST: content-length");
}

static void TestByteByByte()
{
    const std::string raw = MakeJsonRequest();
    http::HttpRequestParser parser;

    std::string pending;
    bool completed = false;

    for (char c : raw)
    {
        pending.push_back(c);

        size_t used = 0;
        const http::ParseStatus status = parser.Parse(pending, used);
        if (used > pending.size())
        {
            Check(false, "byte-by-byte: over consume");
            break;
        }
        pending.erase(0, used);

        if (status == http::ParseStatus::Complete)
        {
            completed = true;
            break;
        }
        if (status == http::ParseStatus::Error) break;
    }

    Check(completed, "byte-by-byte: complete");
    Check(pending.empty(), "byte-by-byte: no leftover");
    Check(parser.Request().body == "{\"model\":\"x\"}", "byte-by-byte: body");
}

static void TestSplitBody()
{
    const std::string body = "{\"model\":\"x\"}";
    const std::string header =
        "POST /v1/chat/completions HTTP/1.1\r\n"
        "Host: gateway.local\r\n"
        "Content-Length: " +
        std::to_string(body.size()) + "\r\n"
        "\r\n";

    const std::string raw = header + body;
    const std::string first = raw.substr(0, header.size() + 3);
    const std::string second = raw.substr(header.size() + 3);

    http::HttpRequestParser parser;
    size_t used = 0;

    const http::ParseStatus first_status = parser.Parse(first, used);
    Check(first_status == http::ParseStatus::NeedMore, "split body: first need more");
    Check(used == first.size(), "split body: header + partial body consumed");

    const http::ParseStatus second_status = parser.Parse(second, used);
    Check(second_status == http::ParseStatus::Complete, "split body: second complete");
    Check(used == second.size(), "split body: second consumed all");
    Check(parser.Request().body == body, "split body: body");
}

static void TestTwoRequests()
{
    const std::string first = "GET /a HTTP/1.1\r\nHost: x\r\n\r\n";
    const std::string second = "GET /b HTTP/1.1\r\nHost: x\r\n\r\n";

    http::HttpRequestParser parser;
    std::string input = first + second;
    size_t used = 0;

    http::ParseStatus status = Feed(parser, input, used);
    Check(status == http::ParseStatus::Complete, "two requests: first complete");
    Check(used == first.size(), "two requests: first consumed");
    Check(input == second, "two requests: second left in buffer");
    Check(parser.Request().target == "/a", "two requests: first target");

    parser.Reset();
    status = Feed(parser, input, used);
    Check(status == http::ParseStatus::Complete, "two requests: second complete");
    Check(input.empty(), "two requests: second consumed");
    Check(parser.Request().target == "/b", "two requests: second target");
}

static bool KeepAliveOf(const std::string &raw)
{
    http::HttpRequestParser parser;
    std::string input = raw;
    size_t used = 0;
    if (Feed(parser, input, used) != http::ParseStatus::Complete) return false;
    return parser.Request().keep_alive;
}

static void TestKeepAlive()
{
    Check(KeepAliveOf("GET / HTTP/1.1\r\nHost: x\r\n\r\n"),
          "keep-alive: HTTP/1.1 default true");
    Check(!KeepAliveOf("GET / HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n"),
          "keep-alive: HTTP/1.1 close");
    Check(!KeepAliveOf("GET / HTTP/1.0\r\nHost: x\r\n\r\n"),
          "keep-alive: HTTP/1.0 default false");
    Check(KeepAliveOf("GET / HTTP/1.0\r\nHost: x\r\nConnection: keep-alive\r\n\r\n"),
          "keep-alive: HTTP/1.0 keep-alive");

    http::HttpRequestParser parser;
    std::string input = "GET / HTTP/1.0\r\nHost: x\r\nConnection: Keep-Alive, Upgrade\r\n\r\n";
    size_t used = 0;
    const http::ParseStatus status = Feed(parser, input, used);
    Check(status == http::ParseStatus::Complete && parser.Request().minor_version == 0,
          "keep-alive: HTTP/1.0 token parse");
}

static void TestHeaderMap()
{
    http::HeaderMap headers;
    headers.Add("Content-Type", "application/json");
    headers.Add("Set-Cookie", "a=1");
    headers.Add("Set-Cookie", "b=2");
    headers.Add("Connection", "keep-alive, Upgrade");

    const std::string *content_type = headers.Get("content-type");
    Check(content_type != nullptr && *content_type == "application/json",
          "header map: case-insensitive get");
    Check(headers.GetAll("SET-COOKIE").size() == 2, "header map: duplicate values");
    Check(headers.Count("set-cookie") == 2, "header map: duplicate count");
    Check(headers.HasToken("connection", "upgrade"), "header map: token");

    headers.Set("content-type", "text/plain");
    Check(headers.Count("content-type") == 1, "header map: set removes old");
    content_type = headers.Get("content-type");
    Check(content_type != nullptr && *content_type == "text/plain",
          "header map: set value");

    headers.AppendToLastValue("extra");
    const std::string *appended = headers.Get("content-type");
    Check(appended != nullptr && *appended == "text/plain extra",
          "header map: append continuation");

    headers.Remove("set-cookie");
    Check(!headers.Has("set-cookie"), "header map: remove");
}

static void TestHeaderContinuation()
{
    http::HttpRequestParser parser;
    std::string input =
        "GET / HTTP/1.1\r\n"
        "Host: gateway.local\r\n"
        "X-Test: one\r\n"
        " two\r\n"
        "\r\n";
    size_t used = 0;

    const http::ParseStatus status = Feed(parser, input, used);
    const std::string *value = parser.Request().headers.Get("x-test");
    Check(status == http::ParseStatus::Complete &&
              value != nullptr && *value == "one two",
          "header: obs-fold continuation");
}

static void TestBadContentLength()
{
    {
        http::HttpRequestParser parser;
        std::string input =
            "POST / HTTP/1.1\r\n"
            "Content-Length: 1\r\n"
            "Content-Length: 1\r\n"
            "\r\n"
            "x";
        size_t used = 0;
        const http::ParseStatus status = Feed(parser, input, used);
        Check(status == http::ParseStatus::Error &&
                  parser.error() == http::ParseError::kBadContentLength,
              "content-length: duplicate");
    }

    {
        http::HttpRequestParser parser;
        std::string input =
            "POST / HTTP/1.1\r\n"
            "Content-Length: 1\r\n"
            "Transfer-Encoding: chunked\r\n"
            "\r\n";
        size_t used = 0;
        const http::ParseStatus status = Feed(parser, input, used);
        Check(status == http::ParseStatus::Error &&
                  parser.error() == http::ParseError::kBadContentLength,
              "content-length: conflict with transfer-encoding");
    }

    {
        http::HttpRequestParser parser;
        std::string input = "POST / HTTP/1.1\r\nContent-Length: abc\r\n\r\n";
        size_t used = 0;
        const http::ParseStatus status = Feed(parser, input, used);
        Check(status == http::ParseStatus::Error &&
                  parser.error() == http::ParseError::kBadContentLength,
              "content-length: invalid");
    }

    {
        http::HttpRequestParser parser;
        std::string input =
            "POST / HTTP/1.1\r\n"
            "Content-Length: 999999999999999999999999999999999999\r\n"
            "\r\n";
        size_t used = 0;
        const http::ParseStatus status = Feed(parser, input, used);
        Check(status == http::ParseStatus::Error &&
                  parser.error() == http::ParseError::kBadContentLength,
              "content-length: overflow");
    }
}

static void TestUnsupportedChunked()
{
    http::HttpRequestParser parser;
    std::string input =
        "POST / HTTP/1.1\r\n"
        "Transfer-Encoding: chunked\r\n"
        "\r\n";
    size_t used = 0;

    const http::ParseStatus status = Feed(parser, input, used);
    Check(status == http::ParseStatus::Error &&
              parser.error() == http::ParseError::kUnsupportedTransferEncoding,
          "chunked request: unsupported");
}

static void TestLimits()
{
    {
        http::HttpRequestParser parser(1024, 4, 64);
        std::string input = "POST / HTTP/1.1\r\nContent-Length: 10\r\n\r\n";
        size_t used = 0;
        const http::ParseStatus status = Feed(parser, input, used);
        Check(status == http::ParseStatus::Error &&
                  parser.error() == http::ParseError::kBodyTooLarge,
              "limit: body too large");
    }

    {
        http::HttpRequestParser parser(32, 1024, 64);
        std::string input =
            "GET / HTTP/1.1\r\n"
            "X-Long: 0123456789012345678901234567890123456789\r\n"
            "\r\n";
        size_t used = 0;
        const http::ParseStatus status = Feed(parser, input, used);
        Check(status == http::ParseStatus::Error &&
                  parser.error() == http::ParseError::kHeaderTooLarge,
              "limit: header too large");
    }

    {
        http::HttpRequestParser parser;
        std::string input = "GET / HTTP/1.1\r\n";
        for (int i = 0; i < 70; ++i)
        {
            input += "X-" + std::to_string(i) + ": a\r\n";
        }
        input += "\r\n";

        size_t used = 0;
        const http::ParseStatus status = Feed(parser, input, used);
        Check(status == http::ParseStatus::Error, "limit: too many headers");
    }
}

static void TestMalformed()
{
    {
        http::HttpRequestParser parser;
        std::string input = "GET\r\n\r\n";
        size_t used = 0;
        const http::ParseStatus status = Feed(parser, input, used);
        Check(status == http::ParseStatus::Error &&
                  parser.error() == http::ParseError::kMalformed,
              "malformed: request line");
    }

    {
        http::HttpRequestParser parser;
        std::string input = "GET / HTTP/2.0\r\nHost: x\r\n\r\n";
        size_t used = 0;
        const http::ParseStatus status = Feed(parser, input, used);
        Check(status == http::ParseStatus::Error, "malformed: unsupported version");
    }
}

static void TestEmptyAndReset()
{
    {
        http::HttpRequestParser parser;
        size_t used = 123;
        const http::ParseStatus status = parser.Parse("", used);
        Check(status == http::ParseStatus::NeedMore && used == 0,
              "empty input: need more");
    }

    {
        http::HttpRequestParser parser;
        std::string input = "GET / HTTP/1.1\r\nHost: x\r\n\r\n";
        size_t used = 0;
        Check(Feed(parser, input, used) == http::ParseStatus::Complete,
              "reset: first request complete");

        used = 0;
        const http::ParseStatus status = parser.Parse("", used);
        Check(status == http::ParseStatus::Error && used == 0 &&
                  parser.error() == http::ParseError::kMalformed,
              "reset: parse after complete without reset");

        parser.Reset();
        input = "GET /again HTTP/1.1\r\nHost: x\r\n\r\n";
        Check(Feed(parser, input, used) == http::ParseStatus::Complete &&
                  parser.Request().target == "/again",
              "reset: can parse again");
    }
}

int main()
{
    TestSimpleGet();
    TestPostWithBody();
    TestByteByByte();
    TestSplitBody();
    TestTwoRequests();
    TestKeepAlive();
    TestHeaderMap();
    TestHeaderContinuation();
    TestBadContentLength();
    TestUnsupportedChunked();
    TestLimits();
    TestMalformed();
    TestEmptyAndReset();

    std::printf("\n%s (%d failed)\n", g_failed == 0 ? "PASS" : "FAIL", g_failed);
    return g_failed == 0 ? 0 : 1;
}
