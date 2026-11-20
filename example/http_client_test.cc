// HttpClientSession 的回环测试：需要先启动 app/build/http_server_test。
//
//   ./app/build/http_server_test 18889
//   ./app/build/http_client_test 18889

#include "../app/http_client_session.hpp"

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <unistd.h>

struct State
{
    std::mutex mutex;
    std::condition_variable cv;
    bool done = false;
    bool closed = false;
    bool failed = false;
    int status = 0;
    std::string body;
    std::string error;
};

static bool WaitFor(State &state, int timeout_ms)
{
    std::unique_lock<std::mutex> lock(state.mutex);
    return state.cv.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                             [&state]() { return state.done && state.closed; });
}

static bool RunRequest(Eventloop *loop, uint16_t port,
                       const http::HttpRequest &request,
                       int expected_status, const std::string &expected_body)
{
    State state;
    HttpClientSession client(loop, "127.0.0.1", port);

    client.SetResponseCallback([&state](const http::HttpResponse &response) {
        std::lock_guard<std::mutex> lock(state.mutex);
        state.status = response.status;
        state.body = response.body;
        state.done = true;
        state.cv.notify_all();
    });

    client.SetErrorCallback([&state](const std::string &message) {
        std::lock_guard<std::mutex> lock(state.mutex);
        state.error = message;
        state.failed = true;
        state.done = true;
        state.cv.notify_all();
    });

    client.SetClosedCallback([&state]() {
        std::lock_guard<std::mutex> lock(state.mutex);
        state.closed = true;
        state.cv.notify_all();
    });

    if (!client.Send(request))
    {
        std::printf("[FAIL] Send returned false\n");
        return false;
    }

    if (!WaitFor(state, 5000))
    {
        std::printf("[FAIL] timeout\n");
        return false;
    }

    if (state.failed)
    {
        std::printf("[FAIL] %s\n", state.error.c_str());
        return false;
    }

    if (state.status != expected_status || state.body != expected_body)
    {
        std::printf("[FAIL] status=%d body=\"%s\"\n",
                    state.status, state.body.c_str());
        return false;
    }

    return true;
}

int main(int argc, char *argv[])
{
    uint16_t port = 8080;
    if (argc > 1) port = static_cast<uint16_t>(std::atoi(argv[1]));

    setvbuf(stdout, nullptr, _IONBF, 0);

    LoopThread *loop_thread = new LoopThread();
    Eventloop *loop = loop_thread->GetLoop();

    http::HttpRequest health;
    health.method = "GET";
    health.target = "/healthz";

    http::HttpRequest echo;
    echo.method = "POST";
    echo.target = "/echo";
    echo.headers.Add("Content-Type", "application/json");
    echo.body = "{\"a\":1}";

    int failed = 0;
    if (!RunRequest(loop, port, health, 200, "{\"ok\":true}"))
    {
        ++failed;
    }
    else
    {
        std::printf("[ OK ] GET /healthz\n");
    }

    if (!RunRequest(loop, port, echo, 200, "{\"a\":1}"))
    {
        ++failed;
    }
    else
    {
        std::printf("[ OK ] POST /echo\n");
    }

    std::printf("\n%s\n", failed == 0 ? "PASS" : "FAIL");
    std::fflush(stdout);
    _exit(failed == 0 ? 0 : 1);
}
