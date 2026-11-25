#pragma once

#include <cstdint>
#include <cstdlib>
#include <string>

namespace gateway
{

// 网关最小配置。
//
// MVP 先只从环境变量读取 key，避免在这一步引入 YAML/JSON 解析器：
//   GATEWAY_API_KEY   客户端访问网关用的 key
//   GATEWAY_TENANT    可选的租户名，默认 default
//   DEEPSEEK_API_KEY  网关访问 DeepSeek 用的 provider key
struct Config
{
    std::string gateway_key;
    std::string gateway_tenant = "default";
    std::string provider_key;
};

inline bool LoadConfigFromEnv(Config &config, std::string *error = nullptr)
{
    const char *gateway_key = std::getenv("GATEWAY_API_KEY");
    if (gateway_key == nullptr || gateway_key[0] == '\0')
    {
        if (error) *error = "GATEWAY_API_KEY is not set";
        return false;
    }

    const char *provider_key = std::getenv("DEEPSEEK_API_KEY");
    if (provider_key == nullptr || provider_key[0] == '\0')
    {
        if (error) *error = "DEEPSEEK_API_KEY is not set";
        return false;
    }

    config.gateway_key = gateway_key;
    config.provider_key = provider_key;

    const char *tenant = std::getenv("GATEWAY_TENANT");
    if (tenant != nullptr && tenant[0] != '\0')
    {
        config.gateway_tenant = tenant;
    }

    return true;
}

} // namespace gateway
