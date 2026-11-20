#pragma once

#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

// HTTP 消息层的公共数据结构和 header 容器。
//
// 这一层只依赖 STL，不依赖 pico 和网络层；解析器在
// http_request_parser.hpp 里，单独使用 pico。
namespace http
{

namespace detail
{

inline char ToLowerAscii(char c)
{
    if (c >= 'A' && c <= 'Z') return static_cast<char>(c - 'A' + 'a');
    return c;
}

inline std::string ToLowerAscii(std::string_view text)
{
    std::string result;
    result.reserve(text.size());
    for (char c : text) result.push_back(ToLowerAscii(c));
    return result;
}

inline std::string_view TrimOws(std::string_view text)
{
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t'))
    {
        text.remove_prefix(1);
    }
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t'))
    {
        text.remove_suffix(1);
    }
    return text;
}

inline bool EqualsIgnoreCase(std::string_view left, std::string_view right)
{
    if (left.size() != right.size()) return false;
    for (size_t i = 0; i < left.size(); ++i)
    {
        if (ToLowerAscii(left[i]) != ToLowerAscii(right[i])) return false;
    }
    return true;
}

// 按逗号拆 token，去掉首尾 OWS 后做大小写不敏感比较。
// 例如 "keep-alive, Upgrade" 里查找 "upgrade" 返回 true。
inline bool ContainsToken(std::string_view value, std::string_view token)
{
    for (;;)
    {
        const size_t comma = value.find(',');
        const std::string_view item = TrimOws(value.substr(0, comma));
        if (EqualsIgnoreCase(item, token)) return true;
        if (comma == std::string_view::npos) return false;
        value.remove_prefix(comma + 1);
    }
}

} // namespace detail

struct Header
{
    std::string name;  // 统一小写
    std::string value; // 已去掉首尾 OWS
};

// 保留 header 的插入顺序和重复项；Get 返回第一个，GetAll 返回全部。
class HeaderMap
{
public:
    void Add(std::string_view name, std::string_view value)
    {
        _headers.push_back(Header{
            detail::ToLowerAscii(name),
            std::string(detail::TrimOws(value))});
    }

    void Set(std::string_view name, std::string_view value)
    {
        Remove(name);
        Add(name, value);
    }

    const std::string *Get(std::string_view name) const
    {
        const std::string lower = detail::ToLowerAscii(name);
        for (const Header &header : _headers)
        {
            if (header.name == lower) return &header.value;
        }
        return nullptr;
    }

    std::vector<std::string> GetAll(std::string_view name) const
    {
        const std::string lower = detail::ToLowerAscii(name);
        std::vector<std::string> values;
        for (const Header &header : _headers)
        {
            if (header.name == lower) values.push_back(header.value);
        }
        return values;
    }

    bool Has(std::string_view name) const
    {
        return Get(name) != nullptr;
    }

    size_t Count(std::string_view name) const
    {
        const std::string lower = detail::ToLowerAscii(name);
        size_t count = 0;
        for (const Header &header : _headers)
        {
            if (header.name == lower) ++count;
        }
        return count;
    }

    void Remove(std::string_view name)
    {
        const std::string lower = detail::ToLowerAscii(name);
        _headers.erase(
            std::remove_if(_headers.begin(), _headers.end(),
                           [&lower](const Header &header) {
                               return header.name == lower;
                           }),
            _headers.end());
    }

    // pico 遇到 obs-fold 续行时 name == nullptr，把 value 拼到上一个 header。
    bool AppendToLastValue(std::string_view value)
    {
        if (_headers.empty()) return false;
        value = detail::TrimOws(value);
        _headers.back().value.push_back(' ');
        _headers.back().value.append(value.data(), value.size());
        return true;
    }

    bool HasToken(std::string_view name, std::string_view token) const
    {
        const std::string lower = detail::ToLowerAscii(name);
        for (const Header &header : _headers)
        {
            if (header.name == lower && detail::ContainsToken(header.value, token))
            {
                return true;
            }
        }
        return false;
    }

    const std::vector<Header> &All() const { return _headers; }

    void Clear() { _headers.clear(); }

private:
    std::vector<Header> _headers;
};

struct HttpRequest
{
    std::string method;
    std::string target; // 原样保留，包含 query，不做 URL 解码
    int minor_version = 1;
    HeaderMap headers;
    std::string body;
    bool keep_alive = true;

    std::string VersionString() const
    {
        return "HTTP/1." + std::to_string(minor_version);
    }
};

struct HttpResponse
{
    int status = 0;
    std::string reason;
    int minor_version = 1;
    HeaderMap headers;
    std::string body;
    bool keep_alive = true;

    std::string VersionString() const
    {
        return "HTTP/1." + std::to_string(minor_version);
    }
};

enum class ParseStatus
{
    NeedMore,
    Complete,
    Error
};

enum class ParseError
{
    kNone,
    kMalformed,                   // 400
    kHeaderTooLarge,              // 431
    kBodyTooLarge,                // 413
    kUnsupportedTransferEncoding, // 501
    kBadContentLength             // 400
};

} // namespace http
