// 最小代理示例：
//   下游 -> HTTP proxy -> 上游 HTTP server / stunnel
//
// 编译（在 proxy/ 目录下）：
//   make
//
// 启动前必须配置：
//   GATEWAY_API_KEY    客户端访问网关用的 key
//   DEEPSEEK_API_KEY   网关访问 DeepSeek 用的 provider key
//
// 直连本地上游：
//   ./build/http_proxy_test 8081 8080
//
// 指定上游域名：
//   ./build/http_proxy_test 8081 8080 localhost
//
// 走 stunnel（本地明文 -> 远端 TLS）：
//   ./build/http_proxy_test 8081 443 api.deepseek.com 127.0.0.1 18443
//   参数依次是：代理端口、上游 Host 端口、上游域名、连接 IP、连接端口。

#include "../adapter/eventloop_task_runner.hpp"
#include "../adapter/host_resolver.hpp"
#include "../adapter/http_client_upstream.hpp"
#include "../adapter/http_response_sink.hpp"
#include "../adapter/http_server.hpp"
#include "../gateway/auth.hpp"
#include "../gateway/config.hpp"
#include "../proxy/proxy_manager.hpp"

#include <cstdio>
#include <cstdlib>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

static void SendAuthError(
    const std::shared_ptr<http::HttpResponseWriter> &writer,
    const std::shared_ptr<Connection> &conn)
{
    const std::string body =
        "{\"error\":{\"message\":\"invalid api key\","
        "\"type\":\"authentication_error\"}}";

    http::HeaderMap headers;
    headers.Add("Content-Type", "application/json");
    headers.Add("WWW-Authenticate", "Bearer");

    writer->WriteHeader(401, "Unauthorized", headers,
                        http::HttpResponseWriter::BodyMode::kFixed, body.size());
    writer->WriteBody(body.data(), body.size());

    // 不要在当前 async handler 的调用栈里同步 Finish：
    // 那会立刻触发 HttpServerSession::OnResponseFinished，可能和
    // _awaiting_response 的状态机重入。排到下一轮 loop。
    conn->GetLoop()->QueueInLoop([writer]() {
        writer->Finish();
    });
}

int main(int argc, char *argv[])
{
    uint16_t proxy_port = 8081;
    uint16_t upstream_port = 8080;

    if (argc > 1) proxy_port = static_cast<uint16_t>(std::atoi(argv[1]));
    if (argc > 2) upstream_port = static_cast<uint16_t>(std::atoi(argv[2]));

    setvbuf(stdout, nullptr, _IONBF, 0);

    gateway::Config gateway_config;
    std::string config_error;
    if (!gateway::LoadConfigFromEnv(gateway_config, &config_error))
    {
        std::fprintf(stderr, "%s\n", config_error.c_str());
        return 1;
    }

    gateway::Authenticator authenticator;
    authenticator.AddKey(gateway_config.gateway_key,
                         gateway::Tenant{gateway_config.gateway_tenant});

    // Host 头里的域名/端口，和实际 TCP 连接地址可以不同。
    std::string upstream_host = "127.0.0.1";
    if (argc > 3) upstream_host = argv[3];

    std::string connect_host = upstream_host;
    if (argc > 4) connect_host = argv[4];

    uint16_t connect_port = upstream_port;
    if (argc > 5) connect_port = static_cast<uint16_t>(std::atoi(argv[5]));

    // 启动阶段解析一次，后面每个请求都复用这个 IP。
    std::vector<std::string> connect_addresses;
    std::string resolve_error;
    if (!HostResolver::ResolveOnce(connect_host, connect_port,
                                   connect_addresses, &resolve_error))
    {
        std::fprintf(stderr, "%s\n", resolve_error.c_str());
        return 1;
    }
    const std::string connect_ip = connect_addresses.front();

    proxy::ProxyManager manager;
    HttpServer proxy(proxy_port, 0);

    proxy.SetAsyncHandler(
        [&manager, &authenticator, gateway_config,
         upstream_host, upstream_port, connect_ip, connect_port](
            const http::HttpRequest &request,
            std::shared_ptr<http::HttpResponseWriter> writer,
            const std::shared_ptr<Connection> &conn) -> std::function<void()> {
            const gateway::AuthResult auth = authenticator.Authenticate(request);
            if (!auth.ok)
            {
                SendAuthError(writer, conn);
                return {};
            }

            // 网关 key 不进入 ProxySession，也不进入上游请求。
            http::HttpRequest upstream_request = request;
            upstream_request.headers.Remove("authorization");
            upstream_request.headers.Remove("proxy-authorization");

            auto tasks = std::make_shared<EventloopTaskRunner>(conn->GetLoop());
            auto sink = std::make_shared<HttpResponseSink>(std::move(writer));
            auto upstream = std::make_unique<HttpClientUpstream>(
                conn->GetLoop(), upstream_host, upstream_port,
                connect_ip, connect_port);
            upstream->SetUpstreamAuth(
                "Authorization", "Bearer " + gateway_config.provider_key);

            auto session = manager.Start(std::move(upstream), sink,
                                         std::move(upstream_request), tasks);

            return [session, tasks]() {
                session->Cancel();
            };
        });

    std::printf("HTTP proxy listening on 0.0.0.0:%u -> %s:%u (connect %s:%u)\n",
                proxy_port, upstream_host.c_str(), upstream_port,
                connect_ip.c_str(), connect_port);

    proxy.Start();
    return 0;
}
