#pragma once

#include "eventloop.hpp"
#include "socket.hpp"

#include <atomic>
#include <memory>
#include <netinet/tcp.h>
#include <string>
#include <utility>

// Connector：只负责“把一条 TCP 连接连上”，不负责连接建立后的读写。
//
// 状态机：
//   DISCONNECTED --StartConnect()--> CONNECTING --SO_ERROR==0--> CONNECTED
//                                        |
//                                        +----SO_ERROR!=0 / 超时--> FAILED
//
// 用法：
//   Connector c(loop, port, ip);
//   c.SetConnectedCallback([](int fd){ ... });   // fd 的所有权交给回调方
//   c.SetErrorCallback([](int err){ ... });
//   c.StartConnect(3);
//
// 注意：StartConnect/回调/析构都应该在 loop 线程里发生；如果在别的线程调用
// StartConnect，内部会通过 RunInLoop 投递，但对象本身必须活到回调结束。
class Connector
{
public:
    using ConnectedCallback = std::function<void(int fd)>;
    using ErrorCallback = std::function<void(int err)>;

    Connector(Eventloop* loop, uint16_t port, std::string ip)
        : _loop(loop), _port(port), _ip(std::move(ip)),
          _socket(), _channel(nullptr), _timer_id(0),
          _state(DISCONNECTED), _channel_removed(false)
    {}

    ~Connector()
    {
        CancelTimer();
        if (_channel && !_channel_removed)
        {
            _channel->Remove();
            _channel_removed = true;
        }
        if (_socket.Fd() >= 0)
        {
            _socket.Close();
        }
    }

    Connector(const Connector&) = delete;
    Connector& operator=(const Connector&) = delete;

    // 客户端设置成功回调于失败回调，而服务端只设置成功回调
    // 因为connect可能成功可能失败，accpet只会得到成功的
    void SetConnectedCallback(ConnectedCallback cb) { _on_connected = std::move(cb); }
    void SetErrorCallback(ErrorCallback cb) { _on_error = std::move(cb); }

    void StartConnect(int timeout_sec = 3)
    {
        if (timeout_sec <= 0) timeout_sec = 3;
        // TimerWheel 是 60 个槽、1 秒一格；超过 60 秒会回绕到前面的槽提前触发。
        if (timeout_sec > 60) timeout_sec = 60;

        // 保证 socket/channel/epoll_ctl 都在 loop 线程里操作。
        _loop->RunInLoop([this, timeout_sec]() { StartConnectInLoop(timeout_sec); });
    }

    // 停止连接尝试；不触发 error callback。
    void Cancel()
    {
        _loop->RunInLoop([this]() {
            if (_state != CONNECTING) return;

            CancelTimer();
            if (_channel && !_channel_removed)
            {
                _channel->Remove();
                _channel_removed = true;
            }
            if (_socket.Fd() >= 0) _socket.Close();
            _state = FAILED;
        });
    }

private:
    enum State
    {
        DISCONNECTED,
        CONNECTING,
        CONNECTED,
        FAILED
    };

    void StartConnectInLoop(int timeout_sec)
    {
        if (_state != DISCONNECTED) return; // 一次性对象，防止重复 StartConnect
        _state = CONNECTING;

        // 创建客户端socket
        // 这里不用 Socket::CreateClient：它会把“connect 立即失败”的 errno 吃掉。
        // 分三步做，失败时能把真实 errno 传给错误回调。
        if (!_socket.Create())
        {
            Finish(errno);
            return;
        }
        _socket.NonBlock();
        if (!_socket.Connect(_port, _ip))
        {
            Finish(errno);
            return;
        }

        // socket创建成功并发起connect后才创建channel用于监听，并设置channel回调
        _channel = std::make_unique<Channel>(_socket.Fd(), _loop);
        _channel->SetWriteCallback([this]() { HandleWrite(); });
        _channel->SetErrorCallback([this]() { HandleError(); });
        _channel->SetCloseCallback([this]() { HandleError(); });
        // 启动写监听
        _channel->EnableWriteMonitor();

        // 设置连接时长，超时则关闭socket，并调用_on_error
        _timer_id = NextTimerId();
        _loop->AddTimertask(_timer_id, static_cast<uint32_t>(timeout_sec), [this]() {
            if (_state == CONNECTING) Finish(ETIMEDOUT);
        });
    }

    void HandleWrite()
    {
        if (_state != CONNECTING) return;

        // 非阻塞 connect 失败时，socket 同样会变成“可写”；必须查 SO_ERROR
        // 才能知道是真连上了，还是被拒绝/超时/网络不可达。
        int err = 0;
        socklen_t len = sizeof(err);
        if (::getsockopt(_socket.Fd(), SOL_SOCKET, SO_ERROR, &err, &len) < 0)
        {
            err = errno;
        }
        // 可写说明连接成功，把状态改为CONNECTED，并调用_on_connected
        Finish(err);
    }

    void HandleError()
    {
        if (_state != CONNECTING) return;

        int err = 0;
        socklen_t len = sizeof(err);
        if (::getsockopt(_socket.Fd(), SOL_SOCKET, SO_ERROR, &err, &len) < 0)
        {
            err = errno;
        }
        if (err == 0) err = ECONNREFUSED; // EPOLLERR/HUP 但 SO_ERROR 为 0，按连接失败处理
        Finish(err);
    }

    // 调用这个要么失败了，要么成功了
    void Finish(int err)
    {
        if (_state != CONNECTING) return;
        CancelTimer(); // 取消超时关闭

        // 先把旧 channel 从 poller 摘掉，再回调。回调里 TcpClient 会在同一个 fd
        // 上创建 Connection 的新 Channel，不能让它和旧 Channel 在 Poller 的
        // fd->channel 表里打架。
        if (_channel) 
        {
            _channel->Remove();
            _channel_removed = true;
        }

        if (err == 0)
        {
            int fd = _socket.Release(); // 所有权交给回调方
            if (fd < 0)
            {
                _state = FAILED;
                if (_on_error) _on_error(EBADF);
                return;
            }

            int one = 1;
            ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

            _state = CONNECTED;
            // 回调可能销毁 Connector，先拷到局部变量，调用后不再碰 this。
            if (_on_connected) _on_connected(fd);
            return;
        }

        // 走到这里说明超时了或者失败了，调用失败回调
        _socket.Close();
        _state = FAILED;
        if (_on_error) _on_error(err);
    }

    void CancelTimer()
    {
        if (_timer_id != 0)
        {
            _loop->CancelTimertask(_timer_id);
            _timer_id = 0;
        }
    }

    // 和 TcpServer 的 _next_id 错开，避免在同一个 loop 上撞 timer id。
    static uint64_t NextTimerId()
    {
        static std::atomic<uint64_t> next{1ULL << 63};
        return next.fetch_add(1);
    }

    Eventloop* _loop;
    uint16_t _port;
    std::string _ip;

    Socket _socket;
    std::unique_ptr<Channel> _channel;
    uint64_t _timer_id;

    State _state;
    bool _channel_removed; // 防止析构时把 Connection 新挂上的同 fd Channel 删掉

    ConnectedCallback _on_connected; // connect成功回调
    ErrorCallback _on_error; // connect失败回调
};


// 发现这一层多的回调，都是用在给上一层的设置回调(HandleWrite，HandleError)中的