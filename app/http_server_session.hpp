#pragma once

#include "../http/http_request_parser.hpp"
#include "../http/http_response_writer.hpp"
#include "../source/buffer.hpp"
#include "../source/connection.hpp"

#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

// HTTP 服务端的适配层：一条 TCP 连接对应一个 Session。
//
// 这里才同时看到 Connection / Buffer 和 HttpRequest / HttpResponseWriter；
// http/ 下的 codec 仍然不知道 socket 的存在。
class HttpServerSession : public std::enable_shared_from_this<HttpServerSession>
{
public:
    using ConnectionPtr = std::shared_ptr<Connection>;
    using RequestHandler =
        std::function<void(const http::HttpRequest &, http::HttpResponseWriter &)>;
    using CancelCallback = std::function<void()>;
    using AsyncRequestHandler = std::function<CancelCallback(
        const http::HttpRequest &,
        std::shared_ptr<http::HttpResponseWriter>,
        const ConnectionPtr &)>;

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
            auto cancel = std::move(_cancel);
            cancel();
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

            if (used > view.size())
            {
                // 解析失败时 buffer 里可能还有坏数据；先清掉，避免 Shutdown()
                // 又把 _in_buffer 喂回 message callback，造成错误路径递归。
                in->MoveReadIdx(in->AbleReadSpace());
                SendParseError(http::ParseError::kMalformed);
                return;
            }

            in->MoveReadIdx(used);
            view.remove_prefix(used);

            if (status == http::ParseStatus::NeedMore) return;
            if (status == http::ParseStatus::Error)
            {
                in->MoveReadIdx(in->AbleReadSpace());
                SendParseError(_parser.error());
                return;
            }

            const bool keep_alive = _parser.Request().keep_alive;

            if (_async_handler)
            {
                http::HttpRequest request = _parser.Request();
                auto writer = std::make_shared<http::HttpResponseWriter>(
                    [conn = _conn](const char *data, size_t len) {
                        conn->Send(data, len);
                    },
                    keep_alive);

                _awaiting_response = true;
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

            http::HttpResponseWriter writer(
                [conn = _conn](const char *data, size_t len) {
                    conn->Send(data, len);
                },
                keep_alive);

            if (_handler)
            {
                _handler(_parser.Request(), writer);
            }

            // handler 忘了写响应时兜底一个 500，避免连接空等。
            if (!writer.header_sent())
            {
                writer.WriteHeader(500, "Internal Server Error", http::HeaderMap{},
                                   http::HttpResponseWriter::BodyMode::kNone);
            }
            writer.Finish();

            _parser.Reset();

            if (!keep_alive)
            {
                // 关闭前清掉可能残留的 pipelined 字节，避免 Shutdown() 再触发解析。
                in->MoveReadIdx(in->AbleReadSpace());
                _conn->Shutdown();
                return;
            }

            if (view.empty()) return;
        }
    }

private:
    void OnResponseFinished()
    {
        if (!_awaiting_response) return;

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

    void SendParseError(http::ParseError error)
    {
        int status = 400;
        std::string_view reason = "Bad Request";
        std::string body = "bad request\n";

        switch (error)
        {
        case http::ParseError::kHeaderTooLarge:
            status = 431;
            reason = "Request Header Fields Too Large";
            body = "request header too large\n";
            break;
        case http::ParseError::kBodyTooLarge:
            status = 413;
            reason = "Payload Too Large";
            body = "request body too large\n";
            break;
        case http::ParseError::kUnsupportedTransferEncoding:
            status = 501;
            reason = "Not Implemented";
            body = "transfer-encoding not supported\n";
            break;
        case http::ParseError::kBadContentLength:
        case http::ParseError::kMalformed:
        case http::ParseError::kNone:
            break;
        }

        http::HeaderMap headers;
        headers.Add("Content-Type", "text/plain; charset=utf-8");

        http::HttpResponseWriter writer(
            [conn = _conn](const char *data, size_t len) {
                conn->Send(data, len);
            },
            false);

        writer.WriteHeader(status, reason, headers,
                           http::HttpResponseWriter::BodyMode::kFixed, body.size());
        writer.WriteBody(body.data(), body.size());
        writer.Finish();

        _conn->Shutdown();
    }

    ConnectionPtr _conn;
    http::HttpRequestParser _parser;
    RequestHandler _handler;
    AsyncRequestHandler _async_handler;

    bool _awaiting_response = false;
    bool _keep_alive = true;
    bool _closed = false;
    Buffer *_pending_buffer = nullptr;
    CancelCallback _cancel;
};
