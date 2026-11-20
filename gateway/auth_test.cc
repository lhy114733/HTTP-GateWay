// 网关鉴权和配置的纯单测，不需要 socket。

#include "auth.hpp"
#include "config.hpp"

#include <cstdio>
#include <cstdlib>
#include <string>

namespace
{

int failed = 0;

void Check(bool condition, const char *name)
{
    if (condition)
    {
        std::printf("[ OK ] %s\n", name);
    }
    else
    {
        std::printf("[FAIL] %s\n", name);
        ++failed;
    }
}

http::HttpRequest RequestWithAuth(const std::string &value)
{
    http::HttpRequest request;
    request.method = "GET";
    request.target = "/v1/models";
    request.headers.Set("Authorization", value);
    return request;
}

} // namespace

int main()
{
    gateway::Authenticator auth;
    auth.AddKey("gw-alice", gateway::Tenant{"alice"});
    auth.AddKey("gw-bob", gateway::Tenant{"bob"});

    Check(auth.size() == 2, "two keys added");

    {
        auto result = auth.Authenticate(RequestWithAuth("Bearer gw-alice"));
        Check(result.ok && result.tenant.id == "alice", "valid alice");
    }

    {
        auto result = auth.Authenticate(RequestWithAuth("bearer gw-bob"));
        Check(result.ok && result.tenant.id == "bob", "scheme is case-insensitive");
    }

    {
        auto result = auth.Authenticate(RequestWithAuth("Bearer   gw-alice"));
        Check(result.ok && result.tenant.id == "alice", "extra spaces accepted");
    }

    {
        auto result = auth.Authenticate(http::HttpRequest{});
        Check(!result.ok && !result.error.empty(), "missing header rejected");
    }

    {
        auto result = auth.Authenticate(RequestWithAuth("Bearer wrong"));
        Check(!result.ok && !result.error.empty(), "wrong key rejected");
    }

    {
        auto result = auth.Authenticate(RequestWithAuth("Basic gw-alice"));
        Check(!result.ok, "unsupported scheme rejected");
    }

    {
        auto result = auth.Authenticate(RequestWithAuth("Bearer "));
        Check(!result.ok, "empty token rejected");
    }

    {
        auto result = auth.Authenticate(RequestWithAuth("Bearer"));
        Check(!result.ok, "missing token rejected");
    }

    {
        gateway::Authenticator empty;
        Check(empty.Authenticate(RequestWithAuth("Bearer anything")).ok == false,
              "empty authenticator rejects");
    }

    unsetenv("GATEWAY_API_KEY");
    unsetenv("DEEPSEEK_API_KEY");
    unsetenv("GATEWAY_TENANT");
    {
        gateway::Config config;
        std::string error;
        Check(!gateway::LoadConfigFromEnv(config, &error) && !error.empty(),
              "config requires GATEWAY_API_KEY");
    }

    setenv("GATEWAY_API_KEY", "gw-env", 1);
    unsetenv("DEEPSEEK_API_KEY");
    {
        gateway::Config config;
        std::string error;
        Check(!gateway::LoadConfigFromEnv(config, &error) && !error.empty(),
              "config requires DEEPSEEK_API_KEY");
    }

    setenv("DEEPSEEK_API_KEY", "sk-env", 1);
    {
        gateway::Config config;
        std::string error;
        Check(gateway::LoadConfigFromEnv(config, &error), "config loads keys");
        Check(config.gateway_key == "gw-env", "gateway key loaded");
        Check(config.provider_key == "sk-env", "provider key loaded");
        Check(config.gateway_tenant == "default", "default tenant");
    }

    setenv("GATEWAY_TENANT", "alice", 1);
    {
        gateway::Config config;
        std::string error;
        Check(gateway::LoadConfigFromEnv(config, &error), "config loads tenant");
        Check(config.gateway_tenant == "alice", "tenant loaded");
    }

    unsetenv("GATEWAY_API_KEY");
    unsetenv("DEEPSEEK_API_KEY");
    unsetenv("GATEWAY_TENANT");

    std::printf("\n%s\n", failed == 0 ? "PASS" : "FAIL");
    return failed == 0 ? 0 : 1;
}
