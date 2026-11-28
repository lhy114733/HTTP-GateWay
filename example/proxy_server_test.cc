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
// 指定工作线程数（省略时使用主 Eventloop 处理 I/O）：
//   ./build/http_proxy_test 8081 8080 --threads 4
//
// 指定上游域名：
//   ./build/http_proxy_test 8081 8080 localhost
//
// 走 stunnel（本地明文 -> 远端 TLS）：
//   ./build/http_proxy_test 8081 443 api.deepseek.com 127.0.0.1 18443
//   参数依次是：代理端口、上游 Host 端口、上游域名、连接 IP、连接端口。

#include "../core/net/host_resolver.hpp"
#include "../core/http/http_client.hpp"
#include "../core/http/http_server.hpp"
#include "../gateway/auth.hpp"
#include "../gateway/config.hpp"
#include "../proxy/proxy_manager.hpp"

#include <charconv>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

static void SendAuthError(const std::shared_ptr<http::HttpResponseWriter> &writer, const std::shared_ptr<Connection> &conn)
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

static void PrintUsage(std::FILE *out, const char *program)
{
    std::fprintf(out,
                 "Usage: %s [--threads N] [proxy_port [upstream_port [upstream_host "
                 "[connect_ip [connect_port]]]]]\n"
                 "  --threads N   Number of worker Eventloops (default: 0, main loop handles I/O).\n"
                 "  --help        Show this message.\n",
                 program);
}

int main(int argc, char *argv[])
{
    int worker_threads = 0;
    std::vector<const char *> positional;
    for (int i = 1; i < argc; ++i)
    {
        const std::string_view arg = argv[i];
        if (arg == "--help")
        {
            PrintUsage(stdout, argv[0]);
            return 0;
        }
        if (arg == "--threads")
        {
            if (++i == argc)
            {
                std::fprintf(stderr, "--threads requires a nonnegative integer.\n");
                return 1;
            }

            const std::string_view count = argv[i];
            const auto end = count.data() + count.size();
            const auto result = std::from_chars(count.data(), end, worker_threads);
            if (result.ec != std::errc{} || result.ptr != end || worker_threads < 0)
            {
                std::fprintf(stderr, "Invalid --threads value: %s\n", argv[i]);
                return 1;
            }
            continue;
        }
        if (arg.substr(0, 2) == "--")
        {
            std::fprintf(stderr, "Unknown option: %s\n", argv[i]);
            PrintUsage(stderr, argv[0]);
            return 1;
        }
        positional.push_back(argv[i]);
    }

    if (positional.size() > 5)
    {
        PrintUsage(stderr, argv[0]);
        return 1;
    }

    uint16_t proxy_port = 8081;
    uint16_t upstream_port = 8080;

    if (!positional.empty()) proxy_port = static_cast<uint16_t>(std::atoi(positional[0]));
    if (positional.size() > 1) upstream_port = static_cast<uint16_t>(std::atoi(positional[1]));

    setvbuf(stdout, nullptr, _IONBF, 0);

    // 加载网关最小配置
    gateway::Config gateway_config;
    std::string config_error;
    if (!gateway::LoadConfigFromEnv(gateway_config, &config_error))
    {
        std::fprintf(stderr, "%s\n", config_error.c_str());
        return 1;
    }
    // 增加默认的gateway和tenant用来身份识别
    gateway::Authenticator authenticator;
    authenticator.AddKey(gateway_config.gateway_key, gateway::Tenant{gateway_config.gateway_tenant});

    // Host 头里的域名/端口，和实际 TCP 连接地址可以不同。
    std::string upstream_host = "127.0.0.1";
    if (positional.size() > 2) upstream_host = positional[2];

    std::string connect_host = upstream_host;
    if (positional.size() > 3) connect_host = positional[3];

    uint16_t connect_port = upstream_port;
    if (positional.size() > 4) connect_port = static_cast<uint16_t>(std::atoi(positional[4]));

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

    // 代理管理和服务器
    proxy::ProxyManager manager;
    HttpServer proxy(proxy_port, worker_threads);

    proxy.SetAsyncHandler(
        [&manager, &authenticator, gateway_config,
         upstream_host, upstream_port, connect_ip, connect_port](
            const http::HttpRequest &request,
            std::shared_ptr<http::HttpResponseWriter> writer,
            const std::shared_ptr<Connection> & conn) -> std::function<void()> 
            {
                // 对于收到的报文先身份识别
                const gateway::AuthResult auth = authenticator.Authenticate(request);
                if (!auth.ok)
                {
                    SendAuthError(writer, conn);
                    return {};
                }

                // 处理request报文，处理完给SessionManager.Start->新的ProxySession->完成代理
                // 网关 key 不进入 ProxySession，也不进入上游请求。
                // 走到这里说明身份识别对了，去掉request中的这两个字段
                http::HttpRequest upstream_request = request;
                upstream_request.headers.Remove("authorization");
                upstream_request.headers.Remove("proxy-authorization");
                upstream_request.headers.Set(
                    "Authorization", "Bearer " + gateway_config.provider_key);

                auto upstream = std::make_unique<HttpClient>(
                    conn->GetLoop(), upstream_host, upstream_port,
                    connect_ip, connect_port);

                auto post = [loop = conn->GetLoop()](std::function<void()> task) {
                    loop->QueueInLoop(std::move(task));
                };

                //这一步包含了：新建一个ProxySession，ProxySession给upstream设置了回调并发送请求
                auto session = manager.Start(std::move(upstream), std::move(writer),
                                            std::move(upstream_request), post, conn);

                return [session]() {
                    session->Cancel();
            };
        });

    std::printf("HTTP proxy listening on 0.0.0.0:%u -> %s:%u (connect %s:%u, worker threads: %d)\n",
                proxy_port, upstream_host.c_str(), upstream_port,
                connect_ip.c_str(), connect_port, worker_threads);

    proxy.Start();
    return 0;
}
