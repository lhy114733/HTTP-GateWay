#pragma once

#include "../core/http/http_response_writer.hpp"
#include "../proxy/ports.hpp"

#include <memory>
#include <string_view>

// proxy::DownstreamSink 的 HTTP 实现。
class HttpResponseSink : public proxy::DownstreamSink
{
public:
    explicit HttpResponseSink(std::shared_ptr<http::HttpResponseWriter> writer)
        : _writer(std::move(writer))
    {
    }

    void WriteHeader(int status,
                     std::string_view reason,
                     const http::HeaderMap &headers,
                     proxy::BodyMode mode,
                     size_t content_length) override
    {
        if (!_writer) return;

        http::HttpResponseWriter::BodyMode writer_mode =
            http::HttpResponseWriter::BodyMode::kNone;

        if (mode == proxy::BodyMode::kFixed)
        {
            writer_mode = http::HttpResponseWriter::BodyMode::kFixed;
        }
        else if (mode == proxy::BodyMode::kChunked)
        {
            writer_mode = http::HttpResponseWriter::BodyMode::kChunked;
        }

        _writer->WriteHeader(status, reason, headers, writer_mode, content_length);
    }

    void WriteBody(const char *data, size_t len) override
    {
        if (_writer) _writer->WriteBody(data, len);
    }

    void WriteChunk(const char *data, size_t len) override
    {
        if (_writer) _writer->WriteChunk(data, len);
    }

    void Finish() override
    {
        if (_writer) _writer->Finish();
    }

private:
    std::shared_ptr<http::HttpResponseWriter> _writer;
};
