#pragma once

#include "connector.hpp"
#include "connection.hpp"

#include <utility>

// TcpClient：Connector + Connection 的薄门面。
//
//   TcpClient c(loop, 8888, "127.0.0.1");
//   c.SetMessageCallback([](const std::shared_ptr<Connection>& conn, Buffer* buf) {
//       // 处理收到的字节
//   });
//   c.Connect();
//   c.Send("hello", 5);
//   c.Disconnect();
//
// 注意：TcpClient 不拥有 Eventloop，调用方要保证 loop 比 TcpClient 活得久；
// 回调都在 loop 线程执行。
class TcpClient
{
public:
    using ConnectionPtr = std::shared_ptr<Connection>;
    using ConnectedCallback = std::function<void(const ConnectionPtr&)>;
    using MessageCallback = std::function<void(const ConnectionPtr&, Buffer*)>;
    using ClosedCallback = std::function<void(const ConnectionPtr&)>;
    using ConnectErrorCallback = std::function<void(int err)>;

    TcpClient(Eventloop* loop, uint16_t port, std::string ip)
        : _loop(loop), _connector(loop, port, std::move(ip)),
          _conn(nullptr), _conn_id(0)
    {
        // 设置connect成功与失败的回调
        _connector.SetConnectedCallback(
            std::bind(&TcpClient::HandleNewConnection, this, std::placeholders::_1));
        _connector.SetErrorCallback(
            std::bind(&TcpClient::HandleConnectError, this, std::placeholders::_1));
    }

    void SetConnectedCallback(const ConnectedCallback& cb) { _connected_callback = cb; }
    void SetMessageCallback(const MessageCallback& cb) { _message_callback = cb; }
    void SetClosedCallback(const ClosedCallback& cb) { _closed_callback = cb; }
    void SetConnectErrorCallback(const ConnectErrorCallback& cb) { _connect_error_callback = cb; }

    // 非阻塞：调用后立即返回，成功/失败通过回调通知。
    void Connect(int timeout_sec = 3) { _connector.StartConnect(timeout_sec); }

    // 连接还没建立或已经关闭时返回 false，避免空指针崩溃。
    bool Send(const char* data, size_t len)
    {
        if (!_conn) return false;
        _conn->Send(data, len);
        return true;
    }

    // 优雅关闭：先发完 _out_buffer 里剩下的数据，再 Release。
    void Disconnect()
    {
        if (_conn) _conn->Shutdown();
    }

    // 立即中止：用于下游断开时停止上游。
    void Abort()
    {
        _aborted = true;
        _connector.Cancel();

        if (_conn)
        {
            _conn->Release();
            _conn.reset();
        }
    }

private:
    void HandleNewConnection(int fd) // 最终给HandleWrite调
    {
        if (_aborted)
        {
            ::close(fd);
            return;
        }

        // 创建make_shared<Connection>，设置回调，只是没有SetAnyCallBack了
        _conn = std::make_shared<Connection>(++_conn_id, fd, _loop);
        _conn->SetConnectedCallback(_connected_callback);
        _conn->SetMessageCallback(_message_callback);
        _conn->SetClosedCallback(_closed_callback);
        _conn->Established(); //就绪初始化，主要是调用_connected_callback
    }

    void HandleConnectError(int err)
    {
        if (_connect_error_callback) _connect_error_callback(err);
    }

    Eventloop* _loop;
    Connector _connector;
    ConnectionPtr _conn;
    uint64_t _conn_id;
    bool _aborted = false;

    ConnectedCallback _connected_callback;
    MessageCallback _message_callback;
    ClosedCallback _closed_callback;
    ConnectErrorCallback _connect_error_callback;
};

// Client的connect(socket)和connect成功后connection(一个连接)用的是同一个fd
// connection，connector，acceptor都有一个Socket，只是conector成功的话会共用，acceptor一直独占一个