#pragma once

#include "proxy_session.hpp"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <utility>

namespace proxy
{

// 持有活跃的 ProxySession，直到代理完成或取消。
//
// ProxySession 自己不再自持有；生命周期由 manager 负责。
// manager 用 Post 把“移除 session”排到下一轮 loop，
// 避免在 HttpClientSession 的回调栈里析构 ProxySession。
class ProxyManager
{
public:
    using Post = std::function<void(std::function<void()>)>;

    ProxyManager() = default;

    std::shared_ptr<ProxySession> Start(
        std::unique_ptr<HttpClientSession> upstream,
        std::shared_ptr<http::HttpResponseWriter> downstream,
        http::HttpRequest request,
        Post post = {})
    {
        auto session = std::make_shared<ProxySession>(
            std::move(upstream), std::move(downstream), std::move(request));

        const uint64_t id = _next_id.fetch_add(1);

        {
            std::lock_guard<std::mutex> lock(_mutex);
            _active[id] = session;
        }

        session->SetDoneCallback([this, id, post]() {
            if (post)
            {
                post([this, id]() { Remove(id); });
            }
            else
            {
                Remove(id);
            }
        });

        session->Start();
        return session;
    }

    size_t size() const
    {
        std::lock_guard<std::mutex> lock(_mutex);
        return _active.size();
    }

private:
    void Remove(uint64_t id)
    {
        std::lock_guard<std::mutex> lock(_mutex);
        _active.erase(id);
    }

    mutable std::mutex _mutex;
    std::atomic<uint64_t> _next_id{1};
    std::unordered_map<uint64_t, std::shared_ptr<ProxySession>> _active;
};

} // namespace proxy
