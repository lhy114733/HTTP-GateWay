#pragma once

#include "http_client_session.hpp"

#include "../../proxy/ports.hpp"

#include <memory>
#include <string>
#include <utility>

// proxy::UpstreamClient 的 HTTP 实现。
class HttpUpstreamClient : public proxy::UpstreamClient
{
public:
    // host/host_port 用于 HTTP Host 头；connect_ip/connect_port 用于 TCP 连接。
    // stunnel 模式：host=api.deepseek.com, host_port=443,
    //               connect_ip=127.0.0.1, connect_port=18443。
    HttpUpstreamClient(Eventloop *loop, std::string host, uint16_t host_port,
                       std::string connect_ip, uint16_t connect_port)
        : _client(loop, std::move(host), host_port,
                  std::move(connect_ip), connect_port)
    {
    }

    // 直连：连接地址和 Host 用同一组 host/port。
    HttpUpstreamClient(Eventloop *loop, std::string host, uint16_t port)
        : _client(loop, std::move(host), port)
    {
    }

    // 直连但连接地址单独指定（host 是域名，connect_ip 是解析好的 IP）。
    HttpUpstreamClient(Eventloop *loop, std::string host, uint16_t port,
                       std::string connect_ip)
        : _client(loop, std::move(host), port, std::move(connect_ip))
    {
    }

    // 配置客户端HttpClientSession _client的各种回调，并且发送一次请求request
    bool Start(const http::HttpRequest &request,
               proxy::UpstreamCallbacks callbacks) override
    {
        _callbacks = std::move(callbacks);

        _client.SetHeadersCallback([this](const http::HttpResponse &response) {
            if (_callbacks.on_headers) _callbacks.on_headers(response);
        });
        _client.SetBodyCallback([this](const char *data, size_t len) {
            if (_callbacks.on_body) _callbacks.on_body(data, len);
        });
        _client.SetResponseCallback([this](const http::HttpResponse &response) {
            if (_callbacks.on_complete) _callbacks.on_complete(response);
        });
        _client.SetErrorCallback([this](const std::string &message) {
            if (_callbacks.on_error) _callbacks.on_error(message);
        });
        _client.SetClosedCallback([this]() {
            if (_callbacks.on_closed) _callbacks.on_closed();
        });

        // 发送一次请求，发送请求必须在设置回调之后
        return _client.Send(request); 
    }

    // 由网关控制面配置 provider 的认证头。
    void SetUpstreamAuth(std::string header, std::string value)
    {
        _client.SetUpstreamAuth(std::move(header), std::move(value));
    }

    void Cancel() override
    {
        _client.Cancel();
    }

private:
    HttpClientSession _client;
    proxy::UpstreamCallbacks _callbacks;
};
