#pragma once
#include "../../comm/util.hpp"
#include <sys/epoll.h>


class Eventloop;
// channel模块(描述) + poller模块(组织)
// 记录一个fd所有需要监控的事件、已经就绪的事件，以及fd所有IO事件触发后的操作(回调)
//          **  回调  就是上层模块把下层各个模块耦合起来的方式，
// connection模块通过回调将buffer，channel->poller，(socket?)模块耦合起来
class Channel
{
private:
    int _fd;
    uint32_t _events;
    uint32_t _revents;
    using EventCallBack = std::function<void()>;
    EventCallBack _read_callback;
    EventCallBack _write_callback;
    EventCallBack _error_callback;
    EventCallBack _close_callback;
    EventCallBack _event_callback;

    Eventloop* _loop; //挂载的loop，要在内核中修改和删除监听事件都要通过它
public:
    Channel(int fd,Eventloop* loop) : _loop(loop),_fd(fd), _events(0), _revents(0) {}
    int Fd() { return _fd; }
    // 给上层poller用来设置的函数
    uint32_t Event() { return _events; }
    // void SetEvent(uint32_t events) { _events = events; }
    void SetREvent(uint32_t revents) { _revents = revents; } 

    //当前是否监控了可读
    bool ReadAble() { return (_events & EPOLLIN); } 
    //当前是否监控了可写
    bool WriteAble() { return (_events & EPOLLOUT); }

    // channel自己用的(或者说将来的connection用的)，启动或禁用后，修改自己的event，并推给poller(让poller写到内核中)
    //   相当于主动启动或取消可读或可写监听
    void EnableReadMonitor() { _events |= EPOLLIN; Update(); }
    void EnableWriteMonitor() { _events |= EPOLLOUT; Update(); }
    void DisableReadMonitor() { _events &= ~EPOLLIN; Update(); }
    void DisableWriteMonitor() { _events &= ~EPOLLOUT; Update(); }
    void DisableAllMonitor() { _events = 0; Update(); }

    void SetReadCallback(const EventCallBack &cb) { _read_callback = cb; }
    void SetWriteCallback(const EventCallBack &cb) { _write_callback = cb; }
    void SetErrorCallback(const EventCallBack &cb) { _error_callback = cb; }
    void SetCloseCallback(const EventCallBack &cb) { _close_callback = cb; }
    void SetEventCallback(const EventCallBack &cb) { _event_callback = cb; }

    void Update(); //实际最终就是调poller
    void Remove(); //这个函数到第有用没？？？也没调这个啊
    void HandleEvent()//revent中有就绪事件时调用，eventloop调用
    {
        if ((_revents & EPOLLIN) || (_revents & EPOLLRDHUP) || (_revents & EPOLLPRI))
        {
            if (_read_callback)
                _read_callback();
        }
        /*有可能会释放连接的操作事件，一次只处理一个*/
        if (_revents & EPOLLOUT)
        {
            if (_write_callback)
                _write_callback();
        }
        else if (_revents & EPOLLERR)
        {
            if (_error_callback)
                _error_callback(); // 一旦出错，就会释放连接，因此要放到前边调用任意回调
        }
        else if (_revents & EPOLLHUP)
        {
            if (_close_callback)
                _close_callback();
        }
        if (_event_callback)
            _event_callback();
    }
};
