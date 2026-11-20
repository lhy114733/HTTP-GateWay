#pragma once

#include "../http/http_message.hpp"

#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace gateway
{

struct Tenant
{
    std::string id;
};

struct AuthResult
{
    bool ok = false;
    Tenant tenant;
    std::string error;
};

// 下游网关 key 的校验器。
//
// 客户端发送：
//   Authorization: Bearer gw-xxx
//
// 网关校验通过后，上层会用配置里的 provider key 替换这个 header。
// 这里只负责鉴权，不负责转发。
class Authenticator
{
public:
    void AddKey(std::string key, Tenant tenant)
    {
        if (key.empty()) return;
        _keys.push_back(Entry{std::move(key), std::move(tenant)});
    }

    AuthResult Authenticate(const http::HttpRequest &request) const
    {
        const std::string *header = request.headers.Get("authorization");
        if (header == nullptr)
        {
            return AuthResult{false, {}, "missing authorization header"};
        }

        const std::string_view value(*header);
        std::size_t pos = 0;
        while (pos < value.size() && value[pos] != ' ') ++pos;

        if (pos == 0 || pos == value.size())
        {
            return AuthResult{false, {}, "malformed authorization header"};
        }

        if (!EqualsIgnoreCase(value.substr(0, pos), "Bearer"))
        {
            return AuthResult{false, {}, "unsupported authorization scheme"};
        }

        while (pos < value.size() && value[pos] == ' ') ++pos;
        const std::string_view token = value.substr(pos);
        if (token.empty())
        {
            return AuthResult{false, {}, "empty api key"};
        }

        for (const Entry &entry : _keys)
        {
            if (ConstantTimeEquals(token, entry.key))
            {
                return AuthResult{true, entry.tenant, {}};
            }
        }

        return AuthResult{false, {}, "invalid api key"};
    }

    std::size_t size() const { return _keys.size(); }

private:
    struct Entry
    {
        std::string key;
        Tenant tenant;
    };

    static bool EqualsIgnoreCase(std::string_view left, std::string_view right)
    {
        if (left.size() != right.size()) return false;
        for (std::size_t i = 0; i < left.size(); ++i)
        {
            char a = left[i];
            char b = right[i];
            if (a >= 'A' && a <= 'Z') a = static_cast<char>(a - 'A' + 'a');
            if (b >= 'A' && b <= 'Z') b = static_cast<char>(b - 'A' + 'a');
            if (a != b) return false;
        }
        return true;
    }

    // 避免直接用 == 比较 key；长度不同仍然会提前返回，这是可接受的折中。
    static bool ConstantTimeEquals(std::string_view left, std::string_view right)
    {
        if (left.size() != right.size()) return false;

        unsigned char diff = 0;
        for (std::size_t i = 0; i < left.size(); ++i)
        {
            diff |= static_cast<unsigned char>(left[i] ^ right[i]);
        }
        return diff == 0;
    }

    std::vector<Entry> _keys;
};

} // namespace gateway
