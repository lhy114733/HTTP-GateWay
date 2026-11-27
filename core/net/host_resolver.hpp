#pragma once

#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>

#include <cstdint>
#include <string>
#include <vector>

// 启动阶段的 DNS 解析。
//
// 只在进程启动、Eventloop 还没开始处理请求时调用一次：
//   std::vector<std::string> addresses;
//   HostResolver::ResolveOnce("api.deepseek.com", 80, addresses);
//
// 运行期不要再调用 getaddrinfo：它是阻塞的，会卡住 Eventloop 线程。
// 解析结果由调用方保存，HttpClient 只拿数字 IP 去 connect；
// HTTP 的 Host 头仍然使用域名。
class HostResolver
{
public:
    // 解析 host 的 IPv4 地址，按 getaddrinfo 返回顺序去重后写入 addresses。
    // host 本身是 IPv4 字面量时直接返回，不查 DNS。
    // 失败返回 false，error 非空时写入可读错误。
    static bool ResolveOnce(const std::string &host, uint16_t port,
                            std::vector<std::string> &addresses,
                            std::string *error = nullptr)
    {
        addresses.clear();

        in_addr literal{};
        if (::inet_pton(AF_INET, host.c_str(), &literal) == 1)
        {
            addresses.push_back(host);
            return true;
        }

        addrinfo hints{};
        hints.ai_family = AF_INET; // TcpClient/Socket 目前只支持 IPv4
        hints.ai_socktype = SOCK_STREAM;
        hints.ai_flags = AI_ADDRCONFIG;

        addrinfo *result = nullptr;
        const std::string service = std::to_string(port);
        const int ret =
            ::getaddrinfo(host.c_str(), service.c_str(), &hints, &result);
        if (ret != 0)
        {
            if (error)
            {
                *error = "resolve " + host + " failed: " + ::gai_strerror(ret);
            }
            return false;
        }

        for (addrinfo *item = result; item != nullptr; item = item->ai_next)
        {
            if (item->ai_family != AF_INET || item->ai_addr == nullptr)
            {
                continue;
            }

            char buffer[INET_ADDRSTRLEN] = {0};
            auto *addr = reinterpret_cast<sockaddr_in *>(item->ai_addr);
            if (::inet_ntop(AF_INET, &addr->sin_addr, buffer, sizeof(buffer)) ==
                nullptr)
            {
                continue;
            }

            bool duplicate = false;
            for (const auto &existing : addresses)
            {
                if (existing == buffer)
                {
                    duplicate = true;
                    break;
                }
            }
            if (!duplicate)
            {
                addresses.emplace_back(buffer);
            }
        }

        ::freeaddrinfo(result);

        if (addresses.empty())
        {
            if (error)
            {
                *error = "resolve " + host + " returned no IPv4 address";
            }
            return false;
        }

        return true;
    }
};
