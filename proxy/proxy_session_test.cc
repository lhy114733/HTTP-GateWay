// ProxySession 的纯单测：用 fake upstream / fake downstream，
// 不需要 socket、Eventloop、TcpClient。

#include "proxy_session.hpp"

#include <cstdio>
#include <memory>
#include <string>
#include <utility>

static int g_failed = 0;

static void Check(bool ok, const std::string &name)
{
    std::printf("[%s] %s\n", ok ? " OK " : "FAIL", name.c_str());
    if (!ok) ++g_failed;
}

class FakeUpstream : public proxy::UpstreamClient
{
public:
    bool Start(const http::HttpRequest &request,
               proxy::UpstreamCallbacks callbacks) override
    {
        request_seen = request;
        callbacks_ = std::move(callbacks);
        started = true;
        return true;
    }

    void Cancel() override { canceled = true; }

    void EmitHeaders(const http::HttpResponse &response)
    {
        if (callbacks_.on_headers) callbacks_.on_headers(response);
    }

    void EmitBody(const std::string &body)
    {
        if (callbacks_.on_body) callbacks_.on_body(body.data(), body.size());
    }

    void Complete(const http::HttpResponse &response)
    {
        if (callbacks_.on_complete) callbacks_.on_complete(response);
    }

    void Fail(const std::string &message)
    {
        if (callbacks_.on_error) callbacks_.on_error(message);
    }

    http::HttpRequest request_seen;
    proxy::UpstreamCallbacks callbacks_;
    bool started = false;
    bool canceled = false;
};

class FakeSink : public proxy::DownstreamSink
{
public:
    void WriteHeader(int status, std::string_view,
                     const http::HeaderMap &, proxy::BodyMode mode,
                     size_t content_length) override
    {
        header_sent = true;
        this->status = status;
        this->mode = mode;
        this->content_length = content_length;
    }

    void WriteBody(const char *data, size_t len) override
    {
        body.append(data, len);
    }

    void WriteChunk(const char *data, size_t len) override
    {
        body.append(data, len);
    }

    void Finish() override { finished = true; }

    bool header_sent = false;
    bool finished = false;
    int status = 0;
    proxy::BodyMode mode = proxy::BodyMode::kNone;
    size_t content_length = 0;
    std::string body;
};

static void TestFixedBody()
{
    auto upstream = std::make_unique<FakeUpstream>();
    FakeUpstream *fake = upstream.get();
    auto sink = std::make_shared<FakeSink>();
    int done = 0;

    auto session = std::make_shared<proxy::ProxySession>(
        std::move(upstream), sink, http::HttpRequest{});
    session->SetDoneCallback([&done]() { ++done; });

    Check(session->Start(), "fixed: start");
    Check(fake->started, "fixed: upstream started");

    http::HttpResponse response;
    response.status = 200;
    response.reason = "OK";
    response.headers.Add("Content-Length", "5");
    response.headers.Add("Content-Type", "text/plain");

    fake->EmitHeaders(response);
    Check(sink->header_sent && sink->status == 200 &&
              sink->mode == proxy::BodyMode::kFixed &&
              sink->content_length == 5,
          "fixed: downstream header");

    fake->EmitBody("hello");
    fake->Complete(response);

    Check(sink->body == "hello", "fixed: body");
    Check(sink->finished, "fixed: finished");
    Check(done == 1, "fixed: done callback");
}

static void TestChunkedBody()
{
    auto upstream = std::make_unique<FakeUpstream>();
    FakeUpstream *fake = upstream.get();
    auto sink = std::make_shared<FakeSink>();

    auto session = std::make_shared<proxy::ProxySession>(
        std::move(upstream), sink, http::HttpRequest{});

    session->Start();

    http::HttpResponse response;
    response.status = 200;
    response.headers.Add("Transfer-Encoding", "chunked");

    fake->EmitHeaders(response);
    fake->EmitBody("data: x");
    fake->Complete(response);

    Check(sink->mode == proxy::BodyMode::kChunked, "chunked: mode");
    Check(sink->body == "data: x", "chunked: body");
    Check(sink->finished, "chunked: finished");
}

static void TestError()
{
    auto upstream = std::make_unique<FakeUpstream>();
    FakeUpstream *fake = upstream.get();
    auto sink = std::make_shared<FakeSink>();
    int done = 0;

    auto session = std::make_shared<proxy::ProxySession>(
        std::move(upstream), sink, http::HttpRequest{});
    session->SetDoneCallback([&done]() { ++done; });

    session->Start();
    fake->Fail("connection refused");

    Check(sink->status == 502, "error: 502");
    Check(sink->body.find("connection refused") != std::string::npos,
          "error: message");
    Check(sink->finished, "error: finished");
    Check(done == 1, "error: done callback");
}

static void TestCancel()
{
    auto upstream = std::make_unique<FakeUpstream>();
    FakeUpstream *fake = upstream.get();
    auto sink = std::make_shared<FakeSink>();
    int done = 0;

    auto session = std::make_shared<proxy::ProxySession>(
        std::move(upstream), sink, http::HttpRequest{});
    session->SetDoneCallback([&done]() { ++done; });

    session->Start();
    session->Cancel();
    session->Cancel(); // 幂等

    Check(fake->canceled, "cancel: upstream canceled");
    Check(!sink->finished, "cancel: downstream not finished");
    Check(done == 1, "cancel: done once");
}

int main()
{
    TestFixedBody();
    TestChunkedBody();
    TestError();
    TestCancel();

    std::printf("\n%s (%d failed)\n", g_failed == 0 ? "PASS" : "FAIL", g_failed);
    return g_failed == 0 ? 0 : 1;
}
