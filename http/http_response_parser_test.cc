// HttpResponseParser 的纯字符串测试。
//
// 编译：
//   cd http && make response-parser-test

#include "http_response_parser.hpp"

#include <cstdio>
#include <string>

static int g_failed = 0;

static void Check(bool ok, const std::string &name)
{
    std::printf("[%s] %s\n", ok ? " OK " : "FAIL", name.c_str());
    if (!ok) ++g_failed;
}

static http::ParseStatus Feed(http::HttpResponseParser &parser,
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
            Check(false, "response parser consumed more than input");
            return http::ParseStatus::Error;
        }

        total_consumed += consumed;
        input.erase(0, consumed);

        if (status != http::ParseStatus::NeedMore) return status;
        if (consumed == 0) return status;
    }
}

static void TestFixedBody()
{
    http::HttpResponseParser parser;
    std::string input =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: 2\r\n"
        "\r\n"
        "ok";
    const size_t expected = input.size();
    size_t used = 0;

    const http::ParseStatus status = Feed(parser, input, used);

    Check(status == http::ParseStatus::Complete, "response: fixed complete");
    Check(used == expected && input.empty(), "response: fixed consumed");
    Check(parser.Response().status == 200, "response: status");
    Check(parser.Response().reason == "OK", "response: reason");
    Check(parser.Response().minor_version == 1, "response: version");
    Check(parser.Response().body == "ok", "response: body");
    Check(parser.Response().keep_alive, "response: keep-alive default");

    const std::string *type = parser.Response().headers.Get("content-type");
    Check(type != nullptr && *type == "application/json", "response: header");
}

static void TestByteByByte()
{
    const std::string raw =
        "HTTP/1.1 200 OK\r\n"
        "Content-Length: 5\r\n"
        "\r\n"
        "hello";

    http::HttpResponseParser parser;
    std::string pending;
    bool complete = false;

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
            complete = true;
            break;
        }
        if (status == http::ParseStatus::Error) break;
    }

    Check(complete, "byte-by-byte: complete");
    Check(pending.empty(), "byte-by-byte: no leftover");
    Check(parser.Response().body == "hello", "byte-by-byte: body");
}

static void TestChunked()
{
    http::HttpResponseParser parser;
    std::string input =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/event-stream\r\n"
        "Transfer-Encoding: chunked\r\n"
        "\r\n"
        "7\r\n"
        "data: x\r\n"
        "0\r\n"
        "\r\n";
    const size_t expected = input.size();
    size_t used = 0;

    const http::ParseStatus status = Feed(parser, input, used);

    Check(status == http::ParseStatus::Complete, "chunked: complete");
    Check(used == expected && input.empty(), "chunked: consumed");
    Check(parser.Response().body == "data: x", "chunked: body");
}

static void TestChunkedByteByByte()
{
    const std::string raw =
        "HTTP/1.1 200 OK\r\n"
        "Transfer-Encoding: chunked\r\n"
        "\r\n"
        "7\r\n"
        "data: x\r\n"
        "0\r\n"
        "\r\n";

    http::HttpResponseParser parser;
    std::string pending;
    bool complete = false;

    for (char c : raw)
    {
        pending.push_back(c);

        size_t used = 0;
        const http::ParseStatus status = parser.Parse(pending, used);
        if (used > pending.size())
        {
            Check(false, "chunked byte-by-byte: over consume");
            break;
        }
        pending.erase(0, used);

        if (status == http::ParseStatus::Complete)
        {
            complete = true;
            break;
        }
        if (status == http::ParseStatus::Error) break;
    }

    Check(complete, "chunked byte-by-byte: complete");
    Check(parser.Response().body == "data: x", "chunked byte-by-byte: body");
}

static void TestChunkedCallback()
{
    http::HttpResponseParser parser;
    std::string streamed;
    parser.SetBodyCallback([&streamed](const char *data, size_t len) {
        streamed.append(data, len);
    });

    std::string input =
        "HTTP/1.1 200 OK\r\n"
        "Transfer-Encoding: chunked\r\n"
        "\r\n"
        "7\r\n"
        "data: x\r\n"
        "0\r\n"
        "\r\n";
    size_t used = 0;

    const http::ParseStatus status = Feed(parser, input, used);

    Check(status == http::ParseStatus::Complete, "chunked callback: complete");
    Check(streamed == "data: x", "chunked callback: chunks");
    Check(parser.Response().body.empty(), "chunked callback: not accumulated");
}

static void TestChunkedExtensionAndTrailer()
{
    http::HttpResponseParser parser;
    std::string input =
        "HTTP/1.1 200 OK\r\n"
        "Transfer-Encoding: chunked\r\n"
        "\r\n"
        "5;foo=bar\r\n"
        "hello\r\n"
        "0\r\n"
        "X-Trailer: y\r\n"
        "\r\n";
    size_t used = 0;

    const http::ParseStatus status = Feed(parser, input, used);

    Check(status == http::ParseStatus::Complete, "chunked: trailer complete");
    Check(parser.Response().body == "hello", "chunked: trailer body");
}

static void TestHeadersCallback()
{
    http::HttpResponseParser parser;
    int calls = 0;
    int status = 0;
    const std::string *content_type = nullptr;

    parser.SetHeadersCallback([&](const http::HttpResponse &response) {
        ++calls;
        status = response.status;
        content_type = response.headers.Get("content-type");
    });

    std::string input =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: 2\r\n"
        "\r\n"
        "ok";
    size_t used = 0;

    const http::ParseStatus result = Feed(parser, input, used);
    Check(result == http::ParseStatus::Complete && calls == 1 &&
              status == 200 && content_type != nullptr &&
              *content_type == "application/json",
          "response: headers callback");
}

static void TestTwoResponses()
{
    const std::string first =
        "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok";
    const std::string second =
        "HTTP/1.1 204 No Content\r\n\r\n";

    http::HttpResponseParser parser;
    std::string input = first + second;
    size_t used = 0;

    http::ParseStatus status = Feed(parser, input, used);
    Check(status == http::ParseStatus::Complete, "two responses: first");
    Check(used == first.size() && input == second, "two responses: leftover");
    Check(parser.Response().status == 200, "two responses: first status");

    parser.Reset();
    status = Feed(parser, input, used);
    Check(status == http::ParseStatus::Complete, "two responses: second");
    Check(input.empty() && parser.Response().status == 204,
          "two responses: second status");
}

static void TestNoBodyStatuses()
{
    {
        http::HttpResponseParser parser;
        std::string input =
            "HTTP/1.1 204 No Content\r\n"
            "Content-Length: 100\r\n"
            "\r\n";
        size_t used = 0;
        const http::ParseStatus status = Feed(parser, input, used);
        Check(status == http::ParseStatus::Complete &&
                  parser.Response().body.empty() &&
                  parser.Response().status == 204,
              "no body: 204");
    }

    {
        http::HttpResponseParser parser;
        parser.SetExpectNoBody(true);
        std::string input =
            "HTTP/1.1 200 OK\r\n"
            "Content-Length: 100\r\n"
            "\r\n";
        size_t used = 0;
        const http::ParseStatus status = Feed(parser, input, used);
        Check(status == http::ParseStatus::Complete &&
                  parser.Response().body.empty(),
              "no body: HEAD");
    }
}

static void TestEofBody()
{
    http::HttpResponseParser parser;
    std::string input =
        "HTTP/1.1 200 OK\r\n"
        "Connection: close\r\n"
        "\r\n"
        "hello";
    size_t used = 0;

    const http::ParseStatus status = parser.Parse(input, used);
    Check(status == http::ParseStatus::NeedMore && used == input.size(),
          "eof body: need more");
    Check(parser.Response().body == "hello", "eof body: chunks");
    Check(!parser.Response().keep_alive, "eof body: close");

    const http::ParseStatus eof = parser.OnEof();
    Check(eof == http::ParseStatus::Complete, "eof body: complete on EOF");
}

static void TestTruncated()
{
    http::HttpResponseParser parser;
    std::string input =
        "HTTP/1.1 200 OK\r\n"
        "Content-Length: 5\r\n"
        "\r\n"
        "he";
    size_t used = 0;

    const http::ParseStatus status = Feed(parser, input, used);
    Check(status == http::ParseStatus::NeedMore, "truncated: need more");

    const http::ParseStatus eof = parser.OnEof();
    Check(eof == http::ParseStatus::Error &&
              parser.error() == http::HttpResponseParser::Error::kTruncated,
          "truncated: EOF error");
}

static void TestErrors()
{
    {
        http::HttpResponseParser parser;
        std::string input = "HTTP/1.1 200 OK\r\nContent-Length: abc\r\n\r\n";
        size_t used = 0;
        const http::ParseStatus status = Feed(parser, input, used);
        Check(status == http::ParseStatus::Error &&
                  parser.error() == http::HttpResponseParser::Error::kBadContentLength,
              "error: bad content-length");
    }

    {
        http::HttpResponseParser parser;
        std::string input =
            "HTTP/1.1 200 OK\r\n"
            "Content-Length: 1\r\n"
            "Content-Length: 1\r\n"
            "\r\n";
        size_t used = 0;
        const http::ParseStatus status = Feed(parser, input, used);
        Check(status == http::ParseStatus::Error &&
                  parser.error() == http::HttpResponseParser::Error::kBadContentLength,
              "error: duplicate content-length");
    }

    {
        http::HttpResponseParser parser;
        std::string input =
            "HTTP/1.1 200 OK\r\n"
            "Transfer-Encoding: gzip\r\n"
            "\r\n";
        size_t used = 0;
        const http::ParseStatus status = Feed(parser, input, used);
        Check(status == http::ParseStatus::Error &&
                  parser.error() ==
                      http::HttpResponseParser::Error::kUnsupportedTransferEncoding,
              "error: unsupported transfer-encoding");
    }

    {
        http::HttpResponseParser parser;
        std::string input =
            "HTTP/1.1 200 OK\r\n"
            "Transfer-Encoding: chunked\r\n"
            "\r\n"
            "ZZ\r\n";
        size_t used = 0;
        const http::ParseStatus status = Feed(parser, input, used);
        Check(status == http::ParseStatus::Error &&
                  parser.error() == http::HttpResponseParser::Error::kChunkedError,
              "error: bad chunk size");
    }
}

static void TestLimits()
{
    {
        http::HttpResponseParser parser(32, 1024);
        std::string input =
            "HTTP/1.1 200 OK\r\n"
            "X-Long: 0123456789012345678901234567890123456789\r\n"
            "\r\n";
        size_t used = 0;
        const http::ParseStatus status = Feed(parser, input, used);
        Check(status == http::ParseStatus::Error &&
                  parser.error() == http::HttpResponseParser::Error::kHeaderTooLarge,
              "limit: response header too large");
    }

    {
        http::HttpResponseParser parser(1024, 4);
        std::string input =
            "HTTP/1.1 200 OK\r\n"
            "Content-Length: 10\r\n"
            "\r\n";
        size_t used = 0;
        const http::ParseStatus status = Feed(parser, input, used);
        Check(status == http::ParseStatus::Error &&
                  parser.error() == http::HttpResponseParser::Error::kBodyTooLarge,
              "limit: response body too large");
    }
}

static bool KeepAliveOf(const std::string &raw)
{
    http::HttpResponseParser parser;
    std::string input = raw;
    size_t used = 0;
    if (Feed(parser, input, used) != http::ParseStatus::Complete) return false;
    return parser.Response().keep_alive;
}

static void TestKeepAlive()
{
    Check(KeepAliveOf("HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n"),
          "response keep-alive: 1.1 default");
    Check(!KeepAliveOf("HTTP/1.1 200 OK\r\nContent-Length: 0\r\nConnection: close\r\n\r\n"),
          "response keep-alive: 1.1 close");
    Check(!KeepAliveOf("HTTP/1.0 200 OK\r\nContent-Length: 0\r\n\r\n"),
          "response keep-alive: 1.0 default");
    Check(KeepAliveOf("HTTP/1.0 200 OK\r\nContent-Length: 0\r\nConnection: keep-alive\r\n\r\n"),
          "response keep-alive: 1.0 keep-alive");
}

static void TestEmptyAndReset()
{
    {
        http::HttpResponseParser parser;
        size_t used = 123;
        const http::ParseStatus status = parser.Parse("", used);
        Check(status == http::ParseStatus::NeedMore && used == 0,
              "empty response: need more");
    }

    {
        http::HttpResponseParser parser;
        std::string input = "HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n";
        size_t used = 0;
        Check(Feed(parser, input, used) == http::ParseStatus::Complete,
              "reset: first response");

        used = 0;
        const http::ParseStatus status = parser.Parse("", used);
        Check(status == http::ParseStatus::Error && used == 0,
              "reset: parse after complete without reset");

        parser.Reset();
        input = "HTTP/1.1 204 No Content\r\n\r\n";
        Check(Feed(parser, input, used) == http::ParseStatus::Complete &&
                  parser.Response().status == 204,
              "reset: can parse again");
    }
}

int main()
{
    TestFixedBody();
    TestByteByByte();
    TestChunked();
    TestChunkedByteByByte();
    TestChunkedCallback();
    TestChunkedExtensionAndTrailer();
    TestHeadersCallback();
    TestTwoResponses();
    TestNoBodyStatuses();
    TestEofBody();
    TestTruncated();
    TestErrors();
    TestLimits();
    TestKeepAlive();
    TestEmptyAndReset();

    std::printf("\n%s (%d failed)\n", g_failed == 0 ? "PASS" : "FAIL", g_failed);
    return g_failed == 0 ? 0 : 1;
}
