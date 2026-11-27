#pragma once

#include "http_message.hpp"
#include "picohttpparser.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <system_error>

// 下游 HTTP 请求的增量解析器。
//
// 状态机：
//   kHeaders --header 完整--> kBody --body_remaining == 0--> kComplete
//      |                        |
//      +-- pico -2 --> NeedMore +-- body 不够 --> NeedMore
//      +-- pico -1 / 校验失败 --> kError
//
// Parse() 只消费传进来的 bytes；调用方用 consumed 推进自己的读指针：
//
//   std::string_view view(in->ReadPositon(), in->AbleReadSpace());
//   size_t used = 0;
//   auto status = parser.Parse(view, used);
//   in->MoveReadIdx(used);
//
// Complete 时 Request() 有效；处理完这条请求后必须 Reset()，再解析下一条。
namespace http
{

// 请求解析错误；只由 HttpRequestParser 产生。
enum class RequestParseError
{
    kNone,
    kMalformed,                   // 400
    kHeaderTooLarge,              // 431
    kBodyTooLarge,                // 413
    kUnsupportedTransferEncoding, // 501
    kBadContentLength             // 400
};

class HttpRequestParser
{
private:
    static constexpr size_t kHeaderArraySize = 64;

    enum class State
    {
        kHeaders,
        kBody,
        kComplete,
        kError
    };
    size_t _max_header;
    size_t _max_body;
    size_t _max_headers;

    State _state = State::kHeaders; // 正常状态
    HttpRequest _request; // 最终的报文
    size_t _body_remaining = 0;
    RequestParseError _error = RequestParseError::kNone; // 错误状态，错误

public:
    explicit HttpRequestParser(size_t max_header = 16 * 1024,
                               size_t max_body = 8 * 1024 * 1024,
                               size_t max_headers = kHeaderArraySize)
        : _max_header(max_header),
          _max_body(max_body),
          _max_headers(max_headers > kHeaderArraySize ? kHeaderArraySize : max_headers) // 选大的
    {
    }

    ParseStatus Parse(std::string_view bytes, size_t &consumed)
    {
        consumed = 0;

        if (_state == State::kError) return ParseStatus::Error;
        if (_state == State::kComplete)
        {
            // 调用方忘了 Reset；不能把下一条请求静默丢掉。
            return Fail(RequestParseError::kMalformed, consumed); //有啥用？
        }

        size_t used = 0;

        if (_state == State::kHeaders)
        {
            if (bytes.empty()) return ParseStatus::NeedMore;

            const char *method = nullptr;
            const char *target = nullptr;
            size_t method_len = 0;
            size_t target_len = 0;
            int minor_version = 1;

            std::array<phr_header, kHeaderArraySize> headers{};
            size_t num_headers = _max_headers;
            if (num_headers > headers.size()) num_headers = headers.size();

            // last_len 传 0：pico 是无状态的，每次从 bytes 头重新扫 header。
            // 真正的“只解析一次”由本类的 kBody 状态保证。
            const int ret = phr_parse_request(
                bytes.data(), bytes.size(),
                &method, &method_len,
                &target, &target_len,
                &minor_version,
                headers.data(), &num_headers, 0);

            if (ret == -2)
            {
                // 头部还没解析完时，bytes 全部属于头部前缀；这里不检查的话，
                // 客户端可以一直不发空行，让缓冲区无限增长。
                if (bytes.size() >= _max_header)
                {
                    return Fail(RequestParseError::kHeaderTooLarge, consumed);
                }
                return ParseStatus::NeedMore;
            }
            if (ret == -1) return Fail(RequestParseError::kMalformed, consumed);

            // header总字节总数大于max_header，或http版本大于1.1
            const size_t header_len = static_cast<size_t>(ret);
            if (header_len > _max_header)
            {
                return Fail(RequestParseError::kHeaderTooLarge, consumed);
            }
            if (minor_version != 0 && minor_version != 1)
            {
                return Fail(RequestParseError::kMalformed, consumed);
            }

            // 解析头没问题，填入_request报文请求行和头
            _request.method.assign(method, method_len);
            _request.target.assign(target, target_len);
            _request.minor_version = minor_version;
            _request.headers.Clear();

            for (size_t i = 0; i < num_headers; ++i)
            {
                const std::string_view value(
                    headers[i].value == nullptr ? "" : headers[i].value,
                    headers[i].value_len); // 拿到value

                if (headers[i].name == nullptr)
                {
                    if (!_request.headers.AppendToLastValue(value))
                    {
                        return Fail(RequestParseError::kMalformed, consumed);
                    }
                    continue;
                }
                // 拿到name
                const std::string_view name(headers[i].name, headers[i].name_len);
                _request.headers.Add(name, value);
            }

            // 看看有没有transfer-encoding，content_length_count是否有多个
            const bool has_transfer_encoding = _request.headers.Has("transfer-encoding");
            const size_t content_length_count = _request.headers.Count("content-length");

            // 两者都有->错误
            if (has_transfer_encoding && content_length_count > 0)
            {
                return Fail(RequestParseError::kBadContentLength, consumed);
            }
            // 只有content_length_count，不支持传输编码（chunk）
            if (has_transfer_encoding)
            {
                return Fail(RequestParseError::kUnsupportedTransferEncoding, consumed);
            }
            // content_length_count数量大于2
            if (content_length_count > 1)
            {
                return Fail(RequestParseError::kBadContentLength, consumed);
            }

            // 正好一个content_length，获得body_len
            size_t body_len = 0;
            if (content_length_count == 1)
            {
                const std::string *value = _request.headers.Get("content-length");
                if (value == nullptr || !ParseContentLength(*value, body_len))
                {
                    return Fail(RequestParseError::kBadContentLength, consumed);
                }
                if (body_len > _max_body)
                {
                    return Fail(RequestParseError::kBodyTooLarge, consumed);
                }
            }

            // 设置keeplilve，根据情况添加到_request
            ComputeKeepAlive();

            // 设置used和remaining
            _body_remaining = body_len;
            used += header_len;
            // 该状态
            _state = _body_remaining == 0 ? State::kComplete : State::kBody;
        }

        if (_state == State::kBody)
        {
            const size_t available = bytes.size() - used; //每次重新调用used都会被清0
            // take，body最够则取一个，body不够则取完
            const size_t take = std::min(_body_remaining, available); 

            // 从bytes取body
            if (take > 0)
            {
                _request.body.append(bytes.data() + used, take);
                used += take;
                _body_remaining -= take;
            }

            // 说明一个报文正好取完，修改状态，修改consumed，返回Complete
            if (_body_remaining == 0)
            {
                _state = State::kComplete;
                consumed = used;
                return ParseStatus::Complete;
            }

            // 说明没取完body，状态还在kBody，修改consumed，返回NeedMore
            consumed = used;
            return ParseStatus::NeedMore;
        }

        // 这里只可能是 header 完整且 body 长度为 0 的情况。
        if (_state == State::kComplete)
        {
            consumed = used;
            return ParseStatus::Complete;
        }

        // 防御性写法，增加鲁棒性
        consumed = used;
        return ParseStatus::NeedMore;
    }

    const HttpRequest &Request() const { return _request; } // 获得报文

    RequestParseError error() const { return _error; } // 获得错误状态(错误码)

    void Reset()
    {
        _request = HttpRequest{};
        _state = State::kHeaders;
        _body_remaining = 0;
        _error = RequestParseError::kNone;
    }

private:
    // 主要用于设置错误状态，只要有一个错误，整个连接就废了
    ParseStatus Fail(RequestParseError error, size_t &consumed)
    {
        // 出错时整条连接都会关闭，不需要再让调用方推进 buffer。
        consumed = 0;
        _error = error; // 设置错误状态
        _state = State::kError;
        return ParseStatus::Error; // 返回的是解析状态
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

    void ComputeKeepAlive()
    {
        const bool close = _request.headers.HasToken("connection", "close");

        if (_request.minor_version >= 1)
        {
            // HTTP/1.1 默认 keep-alive，只有显式 close 才关。
            _request.keep_alive = !close;
        }
        else
        {
            // HTTP/1.0 默认 close，只有显式 keep-alive 才复用。
            _request.keep_alive =
                _request.headers.HasToken("connection", "keep-alive") && !close;
        }
    }
};

} // namespace http


// 理解状态机的使用
