#pragma once

#include "http_message.hpp"
#include "picohttpparser.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

// 上游 HTTP 响应解析器。
//
// 和 HttpRequestParser 的区别：
//   - 解析状态行（status / reason）
//   - body 支持 Content-Length、chunked、读到 EOF
//   - 支持流式 body 回调（SSE 用）
//
// 状态机：
//   kHeaders --有 body framing--> kBody / kChunked / kBodyUntilEof --> kComplete
//
// 连接关闭时调用 OnEof()，用于判断“EOF 结束的 body”是否完整，以及是否被截断。
namespace http
{

class HttpResponseParser
{
private:
    static constexpr size_t kHeaderArraySize = 64;
    static constexpr size_t kMaxLineSize = 4 * 1024;

    enum class State
    {
        kHeaders,
        kBody,
        kChunked,
        kBodyUntilEof,
        kComplete,
        kError
    };

    enum class ChunkState
    {
        kSizeLine,
        kData,
        kDataCrlf,
        kTrailers,
        kDone
    };

public:
    using HeadersCallback = std::function<void(const HttpResponse &)>;
    using BodyCallback = std::function<void(const char *, size_t)>;

    enum class Error
    {
        kNone,
        kMalformed,
        kHeaderTooLarge,
        kBodyTooLarge,
        kBadContentLength,
        kUnsupportedTransferEncoding,
        kChunkedError,
        kTruncated
    };

    explicit HttpResponseParser(size_t max_header = 16 * 1024,
                                size_t max_body = 64 * 1024 * 1024)
        : _max_header(max_header), _max_body(max_body)
    {
    }

    // headers 解析完成、body 开始前调用；异步代理用它在 body 前写下游响应头。
    void SetHeadersCallback(HeadersCallback callback)
    {
        _headers_callback = std::move(callback);
    }

    // 设置后 body 不再累加到 Response().body，而是逐块回调（SSE 用）。
    void SetBodyCallback(BodyCallback callback) { _body_callback = std::move(callback); }

    // HEAD 请求的响应没有 body；调用方在 Send(HEAD) 时设置。
    void SetExpectNoBody(bool expect) { _expect_no_body = expect; }

    ParseStatus Parse(std::string_view bytes, size_t &consumed)
    {
        consumed = 0;

        if (_state == State::kError) return ParseStatus::Error;
        if (_state == State::kComplete)
        {
            return Fail(Error::kMalformed, consumed);
        }

        size_t used = 0;

        if (_state == State::kHeaders)
        {
            if (bytes.empty()) return ParseStatus::NeedMore;

            int minor_version = 1;
            int status = 0;
            const char *reason = nullptr;
            size_t reason_len = 0;

            std::array<phr_header, kHeaderArraySize> headers{};
            size_t num_headers = headers.size();

            const int ret = phr_parse_response(
                bytes.data(), bytes.size(),
                &minor_version, &status,
                &reason, &reason_len,
                headers.data(), &num_headers, 0);

            if (ret == -2) return ParseStatus::NeedMore;
            if (ret == -1) return Fail(Error::kMalformed, consumed);

            const size_t header_len = static_cast<size_t>(ret);
            if (header_len > _max_header)
            {
                return Fail(Error::kHeaderTooLarge, consumed);
            }
            if (minor_version != 0 && minor_version != 1)
            {
                return Fail(Error::kMalformed, consumed);
            }

            _response.status = status;
            _response.reason.assign(reason == nullptr ? "" : reason, reason_len);
            _response.minor_version = minor_version;
            _response.headers.Clear();

            for (size_t i = 0; i < num_headers; ++i)
            {
                const std::string_view value(
                    headers[i].value == nullptr ? "" : headers[i].value,
                    headers[i].value_len);

                if (headers[i].name == nullptr)
                {
                    if (!_response.headers.AppendToLastValue(value))
                    {
                        return Fail(Error::kMalformed, consumed);
                    }
                    continue;
                }

                const std::string_view name(headers[i].name, headers[i].name_len);
                _response.headers.Add(name, value);
            }

            ComputeKeepAlive();
            NotifyHeaders();
            used += header_len;

            if (_expect_no_body || IsNoBodyStatus(status))
            {
                _state = State::kComplete;
                consumed = used;
                return ParseStatus::Complete;
            }

            const bool has_transfer_encoding =
                _response.headers.Has("transfer-encoding");
            const bool chunked =
                _response.headers.HasToken("transfer-encoding", "chunked");

            if (has_transfer_encoding && !chunked)
            {
                return Fail(Error::kUnsupportedTransferEncoding, consumed);
            }

            if (chunked)
            {
                _state = State::kChunked;
                ResetChunkedState();
            }
            else
            {
                const size_t content_length_count =
                    _response.headers.Count("content-length");

                if (content_length_count > 1)
                {
                    return Fail(Error::kBadContentLength, consumed);
                }

                if (content_length_count == 1)
                {
                    const std::string *value = _response.headers.Get("content-length");
                    size_t body_len = 0;

                    if (value == nullptr || !ParseContentLength(*value, body_len))
                    {
                        return Fail(Error::kBadContentLength, consumed);
                    }
                    if (_body_callback == nullptr && body_len > _max_body)
                    {
                        return Fail(Error::kBodyTooLarge, consumed);
                    }

                    _body_remaining = body_len;
                    _state = body_len == 0 ? State::kComplete : State::kBody;
                }
                else
                {
                    // 没有 Content-Length / chunked，只能读到连接关闭。
                    _state = State::kBodyUntilEof;
                    _response.keep_alive = false;
                }
            }
        }

        if (_state == State::kComplete)
        {
            consumed = used;
            return ParseStatus::Complete;
        }

        if (_state == State::kBody)
        {
            const size_t available = bytes.size() - used;
            const size_t take = std::min(_body_remaining, available);

            if (take > 0 && !EmitBody(bytes.data() + used, take))
            {
                return Fail(Error::kBodyTooLarge, consumed);
            }

            used += take;
            _body_remaining -= take;

            if (_body_remaining == 0)
            {
                _state = State::kComplete;
                consumed = used;
                return ParseStatus::Complete;
            }

            consumed = used;
            return ParseStatus::NeedMore;
        }

        if (_state == State::kChunked)
        {
            return ParseChunked(bytes, used, consumed);
        }

        if (_state == State::kBodyUntilEof)
        {
            const size_t available = bytes.size() - used;
            if (available > 0 && !EmitBody(bytes.data() + used, available))
            {
                return Fail(Error::kBodyTooLarge, consumed);
            }
            used += available;
            consumed = used;
            return ParseStatus::NeedMore;
        }

        consumed = used;
        return ParseStatus::NeedMore;
    }

    // 连接关闭时调用：处理 EOF 结束的 body，或报告截断。
    ParseStatus OnEof()
    {
        if (_state == State::kComplete) return ParseStatus::Complete;
        if (_state == State::kError) return ParseStatus::Error;

        if (_state == State::kBodyUntilEof)
        {
            _state = State::kComplete;
            return ParseStatus::Complete;
        }

        return Fail(Error::kTruncated);
    }

    const HttpResponse &Response() const { return _response; }

    Error error() const { return _error; }

    void Reset()
    {
        _response = HttpResponse{};
        _state = State::kHeaders;
        _body_remaining = 0;
        _error = Error::kNone;
        _headers_notified = false;
        ResetChunkedState();
    }

private:
    static bool IsNoBodyStatus(int status)
    {
        return (status >= 100 && status < 200) || status == 204 || status == 304;
    }

    ParseStatus Fail(Error error, size_t &consumed)
    {
        consumed = 0;
        _error = error;
        _state = State::kError;
        return ParseStatus::Error;
    }

    ParseStatus Fail(Error error)
    {
        _error = error;
        _state = State::kError;
        return ParseStatus::Error;
    }

    static bool ParseContentLength(std::string_view value, size_t &out)
    {
        value = detail::TrimOws(value);
        if (value.empty()) return false;

        uint64_t parsed = 0;
        const char *begin = value.data();
        const char *end = begin + value.size();
        const auto result = std::from_chars(begin, end, parsed);

        if (result.ec != std::errc() || result.ptr != end) return false;
        if (parsed > static_cast<uint64_t>(std::numeric_limits<size_t>::max()))
        {
            return false;
        }

        out = static_cast<size_t>(parsed);
        return true;
    }

    void NotifyHeaders()
    {
        if (_headers_notified) return;
        _headers_notified = true;
        if (_headers_callback) _headers_callback(_response);
    }

    void ComputeKeepAlive()
    {
        const bool close = _response.headers.HasToken("connection", "close");

        if (_response.minor_version >= 1)
        {
            _response.keep_alive = !close;
        }
        else
        {
            _response.keep_alive =
                _response.headers.HasToken("connection", "keep-alive") && !close;
        }
    }

    void ResetChunkedState()
    {
        _chunk_state = ChunkState::kSizeLine;
        _chunk_remaining = 0;
        _chunk_line.clear();
        _crlf_seen = 0;
    }

    bool EmitBody(const char *data, size_t len)
    {
        if (len == 0) return true;

        if (_body_callback)
        {
            _body_callback(data, len);
            return true;
        }

        if (_response.body.size() > _max_body ||
            len > _max_body - _response.body.size())
        {
            return false;
        }

        _response.body.append(data, len);
        return true;
    }

    ParseStatus ParseChunked(std::string_view bytes, size_t &used, size_t &consumed)
    {
        for (;;)
        {
            if (_chunk_state == ChunkState::kSizeLine ||
                _chunk_state == ChunkState::kTrailers)
            {
                if (used >= bytes.size())
                {
                    consumed = used;
                    return ParseStatus::NeedMore;
                }

                const std::string_view rest = bytes.substr(used);
                const size_t old_size = _chunk_line.size();

                if (old_size > kMaxLineSize)
                {
                    return Fail(Error::kChunkedError, consumed);
                }

                // 不能只在新到的 rest 里找 \r\n：CR 和 LF 可能被拆在两次
                // Parse 调用里，所以要把新字节拼到 _chunk_line 再找。
                const size_t max_copy = kMaxLineSize + 2 - old_size;
                const size_t copy_len = std::min(rest.size(), max_copy);
                _chunk_line.append(rest.data(), copy_len);

                const size_t pos = _chunk_line.find("\r\n");
                if (pos == std::string::npos)
                {
                    if (rest.size() > copy_len || old_size + rest.size() > kMaxLineSize)
                    {
                        return Fail(Error::kChunkedError, consumed);
                    }
                    used = bytes.size();
                    consumed = used;
                    return ParseStatus::NeedMore;
                }

                if (pos > kMaxLineSize || pos + 2 < old_size)
                {
                    return Fail(Error::kChunkedError, consumed);
                }

                used += pos + 2 - old_size;
                _chunk_line.resize(pos);

                if (_chunk_state == ChunkState::kTrailers)
                {
                    if (_chunk_line.empty())
                    {
                        _chunk_state = ChunkState::kDone;
                        _state = State::kComplete;
                        consumed = used;
                        return ParseStatus::Complete;
                    }
                    _chunk_line.clear();
                    continue;
                }

                std::string_view line(_chunk_line);
                const size_t semicolon = line.find(';');
                if (semicolon != std::string_view::npos)
                {
                    line = line.substr(0, semicolon);
                }
                line = detail::TrimOws(line);

                if (line.empty())
                {
                    return Fail(Error::kChunkedError, consumed);
                }

                uint64_t size = 0;
                const char *begin = line.data();
                const char *end = begin + line.size();
                const auto result = std::from_chars(begin, end, size, 16);

                if (result.ec != std::errc() || result.ptr != end)
                {
                    return Fail(Error::kChunkedError, consumed);
                }
                if (size > static_cast<uint64_t>(std::numeric_limits<size_t>::max()))
                {
                    return Fail(Error::kChunkedError, consumed);
                }

                _chunk_line.clear();

                if (size == 0)
                {
                    _chunk_state = ChunkState::kTrailers;
                    continue;
                }

                _chunk_remaining = static_cast<size_t>(size);
                _chunk_state = ChunkState::kData;
                continue;
            }

            if (_chunk_state == ChunkState::kData)
            {
                if (_chunk_remaining == 0)
                {
                    _chunk_state = ChunkState::kDataCrlf;
                    continue;
                }
                if (used >= bytes.size())
                {
                    consumed = used;
                    return ParseStatus::NeedMore;
                }

                const size_t available = bytes.size() - used;
                const size_t take = std::min(_chunk_remaining, available);

                if (!EmitBody(bytes.data() + used, take))
                {
                    return Fail(Error::kBodyTooLarge, consumed);
                }

                used += take;
                _chunk_remaining -= take;

                if (_chunk_remaining == 0) _chunk_state = ChunkState::kDataCrlf;
                continue;
            }

            if (_chunk_state == ChunkState::kDataCrlf)
            {
                while (_crlf_seen < 2 && used < bytes.size())
                {
                    const char expected = _crlf_seen == 0 ? '\r' : '\n';
                    if (bytes[used] != expected)
                    {
                        return Fail(Error::kChunkedError, consumed);
                    }
                    ++used;
                    ++_crlf_seen;
                }

                if (_crlf_seen < 2)
                {
                    consumed = used;
                    return ParseStatus::NeedMore;
                }

                _crlf_seen = 0;
                _chunk_state = ChunkState::kSizeLine;
                continue;
            }

            if (_chunk_state == ChunkState::kDone)
            {
                _state = State::kComplete;
                consumed = used;
                return ParseStatus::Complete;
            }

            return Fail(Error::kChunkedError, consumed);
        }
    }

    size_t _max_header;
    size_t _max_body;

    State _state = State::kHeaders;
    HttpResponse _response;
    size_t _body_remaining = 0;
    Error _error = Error::kNone;

    HeadersCallback _headers_callback;
    BodyCallback _body_callback;
    bool _headers_notified = false;
    bool _expect_no_body = false;

    ChunkState _chunk_state = ChunkState::kSizeLine;
    size_t _chunk_remaining = 0;
    std::string _chunk_line;
    size_t _crlf_seen = 0;
};

} // namespace http
