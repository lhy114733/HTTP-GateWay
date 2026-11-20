#pragma once

#include "http_server_session.hpp"

#include "../../core/net/TcpServer.hpp"

#include <functional>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <utility>

// HTTP 服务端：把 TcpServer 的连接事件转给 HttpServerSession。
//
// 一个连接固定在一个 loop 上；不同连接可能在不同 loop，所以 _sessions 用
// mutex 保护。handler 会被多个 loop 并发调用，业务 handler 需要自己保证线程安全。
class HttpServer
{
public:
    using RequestHandler = HttpServerSession::RequestHandler;
    using AsyncRequestHandler = HttpServerSession::AsyncRequestHandler;

    HttpServer(uint16_t port, int thread_count)
        : _server(port, thread_count)
    {
        _server.SetConnectedCallback(
            std::bind(&HttpServer::HandleConnected, this, std::placeholders::_1));
        _server.SetMessageCallback(
            std::bind(&HttpServer::HandleMessage, this,
                      std::placeholders::_1, std::placeholders::_2));
        _server.SetClosedCallback(
            std::bind(&HttpServer::HandleClosed, this, std::placeholders::_1));
    }

    void SetHandler(RequestHandler handler) { _handler = std::move(handler); }

    void SetAsyncHandler(AsyncRequestHandler handler)
    {
        _async_handler = std::move(handler);
    }

    void Start() { _server.Start(); }

private:
    void HandleConnected(const std::shared_ptr<Connection> &conn)
    {
        auto session = std::make_shared<HttpServerSession>(conn, _handler, _async_handler);

        std::lock_guard<std::mutex> lock(_sessions_mutex);
        _sessions[conn->Id()] = std::move(session);
    }

    void HandleMessage(const std::shared_ptr<Connection> &conn, Buffer *in)
    {
        std::shared_ptr<HttpServerSession> session;
        {
            std::lock_guard<std::mutex> lock(_sessions_mutex);
            const auto it = _sessions.find(conn->Id());
            if (it == _sessions.end()) return;
            session = it->second;
        }

        session->OnData(in);
    }

    void HandleClosed(const std::shared_ptr<Connection> &conn)
    {
        std::shared_ptr<HttpServerSession> session;
        {
            std::lock_guard<std::mutex> lock(_sessions_mutex);
            const auto it = _sessions.find(conn->Id());
            if (it == _sessions.end()) return;
            session = it->second;
            _sessions.erase(it);
        }

        session->OnClosed();
    }

    TcpServer _server;
    RequestHandler _handler;
    AsyncRequestHandler _async_handler;
    std::mutex _sessions_mutex;
    std::unordered_map<int, std::shared_ptr<HttpServerSession>> _sessions;
};
