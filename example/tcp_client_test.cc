// TcpClient 的最小回环测试：
//   1. 起一个线程内的 echo server（监听 127.0.0.1 的临时端口）
//   2. TcpClient 连上去，发 "hello"，校验回显
//   3. 再测一个未监听端口，确认走 connect error 回调
//
// 编译：
//   g++ -std=c++17 -g -O0 -pthread -fsanitize=address,undefined
//       example/tcp_client_test.cc -o build/tcp_client_test -I.
// 运行：
//   ./build/tcp_client_test

#include "source/TcpClient.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>

struct ClientState
{
    std::mutex mutex;
    std::condition_variable cv;
    bool connected = false;
    bool echoed = false;
    bool closed = false;
    bool failed = false;
    int err = 0;
    std::string received;
};

static void EchoServer(int listen_fd)
{
    int conn = ::accept(listen_fd, nullptr, nullptr);
    if (conn < 0) return;

    char buf[4096];
    while (true)
    {
        ssize_t n = ::recv(conn, buf, sizeof(buf), 0);
        if (n <= 0) break;

        ssize_t off = 0;
        while (off < n)
        {
            ssize_t sent = ::send(conn, buf + off, static_cast<size_t>(n - off), MSG_NOSIGNAL);
            if (sent <= 0) break;
            off += sent;
        }
    }
    ::close(conn);
}

static int ListenOnEphemeralPort(uint16_t* port)
{
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    int one = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0 ||
        ::listen(fd, 4) < 0)
    {
        ::close(fd);
        return -1;
    }

    socklen_t len = sizeof(addr);
    if (::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len) < 0)
    {
        ::close(fd);
        return -1;
    }
    *port = ntohs(addr.sin_port);
    return fd;
}

static bool WaitFor(std::condition_variable& cv, std::mutex& mutex,
                    const bool& flag, int timeout_ms)
{
    std::unique_lock<std::mutex> lock(mutex);
    return cv.wait_for(lock, std::chrono::milliseconds(timeout_ms), [&] { return flag; });
}

int main()
{
    setvbuf(stdout, nullptr, _IONBF, 0);

    uint16_t port = 0;
    int listen_fd = ListenOnEphemeralPort(&port);
    if (listen_fd < 0)
    {
        std::printf("[FAIL] listen\n");
        return 1;
    }
    std::thread server(EchoServer, listen_fd);
    server.detach();

    // Eventloop 必须在自己的线程里构造并 Start；LoopThread 负责这件事。
    // 故意 new 出来不 delete：它的 Start() 是死循环，退出时用 _exit。
    LoopThread* loop_thread = new LoopThread();
    Eventloop* loop = loop_thread->GetLoop();

    int failed = 0;

    ClientState st;
    TcpClient client(loop, port, "127.0.0.1");
    client.SetConnectedCallback([&](const std::shared_ptr<Connection>&) {
        std::lock_guard<std::mutex> lock(st.mutex);
        st.connected = true;
        st.cv.notify_all();
    });
    client.SetMessageCallback([&](const std::shared_ptr<Connection>&, Buffer* buf) {
        std::lock_guard<std::mutex> lock(st.mutex);
        size_t n = static_cast<size_t>(buf->AbleReadSpace());
        std::string data(n, '\0');
        buf->Read(&data[0], n);
        st.received += data;
        if (st.received.size() >= 5) st.echoed = true;
        st.cv.notify_all();
    });
    client.SetClosedCallback([&](const std::shared_ptr<Connection>&) {
        std::lock_guard<std::mutex> lock(st.mutex);
        st.closed = true;
        st.cv.notify_all();
    });
    client.SetConnectErrorCallback([&](int err) {
        std::lock_guard<std::mutex> lock(st.mutex);
        st.failed = true;
        st.err = err;
        st.cv.notify_all();
    });

    // Send before connect must not crash.
    if (client.Send("x", 1))
    {
        std::printf("[FAIL] Send before connect returned true\n");
        ++failed;
    }

    client.Connect(3);
    if (!WaitFor(st.cv, st.mutex, st.connected, 3000))
    {
        std::printf("[FAIL] connect timeout, failed=%d errno=%d\n", st.failed ? 1 : 0, st.err);
        ++failed;
    }
    else
    {
        std::printf("[ OK ] connected\n");
        client.Send("hello", 5);
        if (!WaitFor(st.cv, st.mutex, st.echoed, 3000) || st.received != "hello")
        {
            std::printf("[FAIL] echo, got=\"%s\"\n", st.received.c_str());
            ++failed;
        }
        else
        {
            std::printf("[ OK ] echo: %s\n", st.received.c_str());
        }

        client.Disconnect();
        if (!WaitFor(st.cv, st.mutex, st.closed, 3000))
        {
            std::printf("[FAIL] closed callback not called\n");
            ++failed;
        }
        else
        {
            std::printf("[ OK ] closed\n");
        }
    }

    // 未监听端口：应走 connect error，而不是 connected。
    uint16_t dead_port = 0;
    int tmp_fd = ListenOnEphemeralPort(&dead_port);
    if (tmp_fd >= 0) ::close(tmp_fd); // 关掉，端口就没人监听了

    ClientState err_st;
    TcpClient bad_client(loop, dead_port, "127.0.0.1");
    bad_client.SetConnectedCallback([&](const std::shared_ptr<Connection>&) {
        std::lock_guard<std::mutex> lock(err_st.mutex);
        err_st.connected = true;
        err_st.cv.notify_all();
    });
    bad_client.SetConnectErrorCallback([&](int err) {
        std::lock_guard<std::mutex> lock(err_st.mutex);
        err_st.failed = true;
        err_st.err = err;
        err_st.cv.notify_all();
    });
    bad_client.Connect(2);

    if (!WaitFor(err_st.cv, err_st.mutex, err_st.failed, 3000) || err_st.connected)
    {
        std::printf("[FAIL] refused port should call error callback, got connected=%d failed=%d\n",
                    err_st.connected ? 1 : 0, err_st.failed ? 1 : 0);
        ++failed;
    }
    else
    {
        std::printf("[ OK ] connect error: errno=%d\n", err_st.err);
    }

    std::printf("\n%s\n", failed == 0 ? "PASS" : "FAIL");
    std::fflush(stdout);
    _exit(failed == 0 ? 0 : 1);
}
