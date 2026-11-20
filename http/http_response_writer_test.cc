// HttpResponseWriter 的纯字符串测试：不依赖 socket、Buffer、TcpServer。
//
// 编译：
//   cd http && make response-writer-test

#include "http_response_writer.hpp"

#include <cstdio>
#include <string>

static int g_failed = 0;

static void Check(bool ok, const std::string &name)
{
    std::printf("[%s] %s\n", ok ? " OK " : "FAIL", name.c_str());
    if (!ok) ++g_failed;
}

static void TestFixedBody()
{
    std::string out;
    http::HttpResponseWriter writer(
        [&out](const char *data, size_t len) { out.append(data, len); }, true);

    http::HeaderMap headers;
    headers.Add("Content-Type", "application/json");
    headers.Add("Content-Length", "999"); // 应被 writer 忽略
    headers.Add("Connection", "close");   // 应被 writer 忽略

    writer.WriteHeader(200, "OK", headers,
                       http::HttpResponseWriter::BodyMode::kFixed, 2);
    writer.WriteBody("ok", 2);
    writer.Finish();

    const std::string expected =
        "HTTP/1.1 200 OK\r\n"
        "content-type: application/json\r\n"
        "Content-Length: 2\r\n"
        "Connection: keep-alive\r\n"
        "\r\n"
        "ok";

    Check(out == expected, "fixed: exact bytes");
    Check(writer.finished(), "fixed: finished");
}

static void TestNoBody()
{
    std::string out;
    http::HttpResponseWriter writer(
        [&out](const char *data, size_t len) { out.append(data, len); }, false);

    http::HeaderMap headers;
    writer.WriteHeader(204, "", headers,
                       http::HttpResponseWriter::BodyMode::kNone);
    writer.WriteBody("ignored", 7);
    writer.Finish();

    const std::string expected =
        "HTTP/1.1 204 No Content\r\n"
        "Connection: close\r\n"
        "\r\n";

    Check(out == expected, "no body: exact bytes");
}

static void TestChunked()
{
    std::string out;
    http::HttpResponseWriter writer(
        [&out](const char *data, size_t len) { out.append(data, len); }, true);

    http::HeaderMap headers;
    headers.Add("Content-Type", "text/event-stream");
    headers.Add("Transfer-Encoding", "chunked"); // 应被 writer 忽略

    writer.WriteHeader(200, "OK", headers,
                       http::HttpResponseWriter::BodyMode::kChunked);
    writer.WriteChunk("data: x", 7);
    writer.WriteChunk("", 0); // 空 chunk 应忽略
    writer.Finish();
    writer.Finish(); // 幂等

    const std::string expected =
        "HTTP/1.1 200 OK\r\n"
        "content-type: text/event-stream\r\n"
        "Transfer-Encoding: chunked\r\n"
        "Connection: keep-alive\r\n"
        "\r\n"
        "7\r\n"
        "data: x\r\n"
        "0\r\n"
        "\r\n";

    Check(out == expected, "chunked: exact bytes");
}

static void TestChunkedWithSmallChunks()
{
    std::string out;
    http::HttpResponseWriter writer(
        [&out](const char *data, size_t len) { out.append(data, len); }, true);

    writer.WriteHeader(200, "", http::HeaderMap{},
                       http::HttpResponseWriter::BodyMode::kChunked);
    writer.WriteChunk("a", 1);
    writer.WriteChunk("bc", 2);
    writer.Finish();

    const std::string expected =
        "HTTP/1.1 200 OK\r\n"
        "Transfer-Encoding: chunked\r\n"
        "Connection: keep-alive\r\n"
        "\r\n"
        "1\r\n"
        "a\r\n"
        "2\r\n"
        "bc\r\n"
        "0\r\n"
        "\r\n";

    Check(out == expected, "chunked: multiple chunks");
}

static void TestOrderAndManagedHeaders()
{
    std::string out;
    http::HttpResponseWriter writer(
        [&out](const char *data, size_t len) { out.append(data, len); }, false);

    http::HeaderMap headers;
    headers.Add("X-Test", "1");
    headers.Add("Connection", "keep-alive");
    headers.Add("Content-Length", "100");
    headers.Add("Transfer-Encoding", "chunked");
    headers.Add("X-After", "2");

    writer.WriteHeader(200, "", headers,
                       http::HttpResponseWriter::BodyMode::kFixed, 0);
    writer.Finish();

    const std::string expected =
        "HTTP/1.1 200 OK\r\n"
        "x-test: 1\r\n"
        "x-after: 2\r\n"
        "Content-Length: 0\r\n"
        "Connection: close\r\n"
        "\r\n";

    Check(out == expected, "managed headers are skipped");
}

static void TestFinishCallback()
{
    std::string out;
    int calls = 0;
    http::HttpResponseWriter writer(
        [&out](const char *data, size_t len) { out.append(data, len); }, true);
    writer.SetFinishCallback([&calls]() { ++calls; });

    writer.WriteHeader(204, "", http::HeaderMap{},
                       http::HttpResponseWriter::BodyMode::kNone);
    writer.Finish();
    writer.Finish();

    Check(calls == 1, "finish callback: once");
}

static void TestNoHeader()
{
    std::string out;
    http::HttpResponseWriter writer(
        [&out](const char *data, size_t len) { out.append(data, len); }, true);

    writer.WriteBody("x", 1);
    writer.WriteChunk("x", 1);
    writer.Finish();

    Check(out.empty(), "no header: no output");
}

static void TestWriteAfterFinish()
{
    std::string out;
    http::HttpResponseWriter writer(
        [&out](const char *data, size_t len) { out.append(data, len); }, true);

    writer.WriteHeader(200, "", http::HeaderMap{},
                       http::HttpResponseWriter::BodyMode::kFixed, 1);
    writer.WriteBody("a", 1);
    writer.Finish();
    const std::string before = out;

    writer.WriteBody("b", 1);
    writer.Finish();

    Check(out == before, "after finish: ignored");
}

int main()
{
    TestFixedBody();
    TestNoBody();
    TestChunked();
    TestChunkedWithSmallChunks();
    TestOrderAndManagedHeaders();
    TestFinishCallback();
    TestNoHeader();
    TestWriteAfterFinish();

    std::printf("\n%s (%d failed)\n", g_failed == 0 ? "PASS" : "FAIL", g_failed);
    return g_failed == 0 ? 0 : 1;
}
