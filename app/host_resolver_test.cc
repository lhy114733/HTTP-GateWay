// HostResolver 的启动阶段解析测试。
// 只测试本机可确定的结果，不依赖外部网络：
//   - IPv4 字面量不查 DNS
//   - localhost 能解析出 127.0.0.1
//   - .invalid 域名解析失败

#include "host_resolver.hpp"

#include <cstdio>
#include <string>
#include <vector>

static bool Contains(const std::vector<std::string> &addresses,
                     const std::string &expected)
{
    for (const auto &address : addresses)
    {
        if (address == expected) return true;
    }
    return false;
}

int main()
{
    int failed = 0;
    std::vector<std::string> addresses;
    std::string error;

    if (!HostResolver::ResolveOnce("127.0.0.1", 80, addresses, &error))
    {
        std::printf("[FAIL] literal resolve: %s\n", error.c_str());
        ++failed;
    }
    else if (addresses.size() != 1 || addresses.front() != "127.0.0.1")
    {
        std::printf("[FAIL] literal returned unexpected addresses\n");
        ++failed;
    }
    else
    {
        std::printf("[ OK ] IPv4 literal\n");
    }

    error.clear();
    if (!HostResolver::ResolveOnce("localhost", 80, addresses, &error))
    {
        std::printf("[FAIL] localhost resolve: %s\n", error.c_str());
        ++failed;
    }
    else if (!Contains(addresses, "127.0.0.1"))
    {
        std::printf("[FAIL] localhost did not return 127.0.0.1\n");
        ++failed;
    }
    else
    {
        std::printf("[ OK ] localhost -> %s\n", addresses.front().c_str());
    }

    error.clear();
    if (HostResolver::ResolveOnce("host.invalid", 80, addresses, &error))
    {
        std::printf("[FAIL] host.invalid should not resolve\n");
        ++failed;
    }
    else if (error.empty())
    {
        std::printf("[FAIL] resolve failure did not set error\n");
        ++failed;
    }
    else
    {
        std::printf("[ OK ] host.invalid rejected\n");
    }

    std::printf("\n%s\n", failed == 0 ? "PASS" : "FAIL");
    return failed == 0 ? 0 : 1;
}
