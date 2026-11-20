#pragma once

#include "../proxy/ports.hpp"
#include "../source/eventloop.hpp"

#include <functional>
#include <utility>

class EventloopTaskRunner : public proxy::TaskRunner
{
public:
    explicit EventloopTaskRunner(Eventloop *loop) : _loop(loop) {}

    void Post(std::function<void()> task) override
    {
        _loop->QueueInLoop(std::move(task));
    }

private:
    Eventloop *_loop;
};
