#pragma once

#include "http_request_parser.hpp"
#include "http_response_writer.hpp"
#include "../net/buffer.hpp"
#include "../net/connection.hpp"

#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

// HTTP 服务端的适配层：一条 TCP 连接对应一个 Session。
//
// 这里才同时看到 Connection / Buffer 和 HttpRequest / HttpResponseWriter；
// core/http 下的 codec 仍然不知道 socket 的存在。
class HttpServerSession : public std::enable_shared_from_this<HttpServerSession>
{
public:
    using ConnectionPtr = std::shared_ptr<Connection>;
    using RequestHandler = std::function<void(const http::HttpRequest &, http::HttpResponseWriter &)>;
    using CancelCallback = std::function<void()>;
    using AsyncRequestHandler = std::function<CancelCallback(
        const http::HttpRequest &,
        std::shared_ptr<http::HttpResponseWriter>,
        const ConnectionPtr &)>;

private:
    ConnectionPtr _conn; // 管理的链接
    http::HttpRequestParser _parser;
    RequestHandler _handler; // 同步处理
    AsyncRequestHandler _async_handler; // 异步处理

    bool _awaiting_response = false; // 当前是否正在等待一个异步响应完成
    bool _keep_alive = true; // 保存当前异步请求的 keep-alive 结果
    bool _closed = false; // 表示 session 是否已经关闭
    Buffer *_pending_buffer = nullptr; // 已发出但尚未收到响应”的 I/O 操作,指向连接当前的输入 Buffer
    CancelCallback _cancel;

public:

    HttpServerSession(ConnectionPtr conn, RequestHandler handler,
                      AsyncRequestHandler async_handler = {})
        : _conn(std::move(conn)), _handler(std::move(handler)),
          _async_handler(std::move(async_handler))
    {
    }

    ~HttpServerSession()
    {
        OnClosed();
    }

    // 下游连接关闭时由 HttpServer 调用；也可以由析构兜底。
    void OnClosed()
    {
        if (_closed) return;
        _closed = true;

        _awaiting_response = false;
        _pending_buffer = nullptr;

        if (_cancel)
        {
            _cancel();
        }
    }

    void OnData(Buffer *in)
    {
        if (in == nullptr) return;

        // 上一次异步响应还没结束，先把新字节留在 Buffer 里，等响应完成后再解析。
        if (_awaiting_response)
        {
            _pending_buffer = in;
            return;
        }

        std::string_view view(in->ReadPositon(), static_cast<size_t>(in->AbleReadSpace()));

        for (;;)
        {
            size_t used = 0;
            const http::ParseStatus status = _parser.Parse(view, used);

            in->MoveReadIdx(used);
            view.remove_prefix(used); // 移除前缀

            if (status == http::ParseStatus::NeedMore) return;
            if (status == http::ParseStatus::Error)
            {
                in->MoveReadIdx(in->AbleReadSpace());
                SendParseError(_parser.error());
                return;
            }

            // 到这里说明至少解析出来了一个报文
            const bool keep_alive = _parser.Request().keep_alive;

            if (_async_handler)
            {
                http::HttpRequest request = _parser.Request();
                // 异步的writer必须活着，所以不能放到栈上，用智能指针
                auto writer = std::make_shared<http::HttpResponseWriter>(
                    [conn = _conn](const char *data, size_t len) {
                        conn->Send(data, len);
                    },
                    keep_alive);

                _awaiting_response = true; // 异步等待标志
                _keep_alive = keep_alive;
                _pending_buffer = in;
                _parser.Reset();

                std::weak_ptr<HttpServerSession> weak = shared_from_this();
                writer->SetFinishCallback([weak]() {
                    if (auto self = weak.lock()) self->OnResponseFinished();
                });

                _cancel = _async_handler(request, writer, _conn);
                return;
            }

            // 给同步handler用的writer
            http::HttpResponseWriter writer(
                [conn = _conn](const char *data, size_t len) {
                    conn->Send(data, len);
                },
                keep_alive);
            // 执行用户回调，这个回调必须完成报文发送
            if (_handler)
            {
                _handler(_parser.Request(), writer); 
                // 第一个参数是接收到的http请求报文，对这个报文做处理， 
                // 第二个参数决定向哪个_conn里写，因为WriteHeader()和WriteBody()就是Send()
            }
            // 发送完了，调用发送结束
            writer.Finish();

            // 重置_parser才可以解析下一个报文
            _parser.Reset();

            // 对端不支持长连接的话，处理一个报文直接关闭连接
            if (!keep_alive)
            {
                // 关闭前清掉可能残留的 pipelined 字节，避免 Shutdown() 再触发解析。
                // 因为对端可能不遵守短链接，一下发了好几个报文
                in->MoveReadIdx(in->AbleReadSpace());
                _conn->Shutdown();
                return;
            }

            // 正好处理完整数个报文的情况
            if (view.empty()) return;
        }
    }

private:
    void OnResponseFinished()
    {
        // 超限关闭后保留取消函数，让 OnClosed 停止上游。
        if (!_awaiting_response || _conn->OutputAborted()) return;

        _awaiting_response = false;
        _cancel = nullptr;

        Buffer *in = _pending_buffer;
        _pending_buffer = nullptr;

        if (_closed) return;

        if (!_keep_alive)
        {
            if (in != nullptr) in->MoveReadIdx(in->AbleReadSpace());
            _conn->Shutdown();
            return;
        }

        // 响应期间可能又收到了下一条请求；现在继续解析。
        if (in != nullptr) OnData(in);
    }

    // 客户端错误，发送400响应报文
    void SendParseError(http::RequestParseError error)
    {
        // 设置响应报文的参数
        int status = 400;
        std::string_view reason = "Bad Request";
        std::string body = "bad request\n";
        switch (error)
        {
        case http::RequestParseError::kHeaderTooLarge:
            status = 431;
            reason = "Request Header Fields Too Large";
            body = "request header too large\n";
            break;
        case http::RequestParseError::kBodyTooLarge:
            status = 413;
            reason = "Payload Too Large";
            body = "request body too large\n";
            break;
        case http::RequestParseError::kUnsupportedTransferEncoding:
            status = 501;
            reason = "Not Implemented";
            body = "transfer-encoding not supported\n";
            break;
        case http::RequestParseError::kBadContentLength:
        case http::RequestParseError::kMalformed:
        case http::RequestParseError::kNone:
            break;
        }

        http::HeaderMap headers;
        headers.Add("Content-Type", "text/plain; charset=utf-8");

        // 发送做好的响应报文
        http::HttpResponseWriter writer(
            [conn = _conn](const char *data, size_t len) {
                conn->Send(data, len); // 设置writer的发送方式，实现了耦合，连接起来了http和tcp
            },
            false);

        writer.WriteHeader(status, reason, headers,
                           http::HttpResponseWriter::BodyMode::kFixed, body.size());
        writer.WriteBody(body.data(), body.size());
        writer.Finish();

        // 通知tcp，关闭连接
        _conn->Shutdown();
    }
};
