#pragma once

#include <cstdint>
#include <functional>
#include <vector>
#include <memory>

#include <sys/timerfd.h>

#include "channel.hpp"
// #include "eventloop.hpp"

using TaskCallBack = std::function<void()>;
using ReleaseCallBack = std::function<void()>;
class TimerTask 
{
private:
    uint64_t _id;
    uint32_t _timeout;
    bool _canceled; //没有取消才被执行
    TaskCallBack _timertask;
    ReleaseCallBack _release; //这个什么时候设置了？？？TimerWheel连个接口都没留
public:
    TimerTask(uint64_t id,uint32_t timeout,TaskCallBack timertask):_id(id),_timeout(timeout),_timertask(timertask),_canceled(false) {}
    uint64_t Id() { return _id; }
    uint32_t Timeoout() { return _timeout; }
    void SetRelease(const ReleaseCallBack &cb) { _release = cb; }
    void SetCanceled() { _canceled = true; }
    ~TimerTask() { 
        if( _canceled == false ) _timertask(); //没有取消才被执行 
        _release(); } // 存在的意义是什么？？？取消都取消不了？？？
};

class Eventloop;
class TimerWheel 
{
private:
    int _tick;
    int _size;
    std::vector<std::vector<std::shared_ptr<TimerTask>>> _wheel;
    std::unordered_map<uint64_t,std::weak_ptr<TimerTask>> _timers;

    Eventloop* _loop;
    int _timerfd;
    Channel _timer_channel;
private:
    int CreatTimer()
    {
        int timerfd = timerfd_create(CLOCK_MONOTONIC, 0);
        if (timerfd < 0) {
            perror("timerfd_create error");
            return -1;
        }
        struct itimerspec itime;
        itime.it_value.tv_sec = 1;
        itime.it_value.tv_nsec = 0;//第一次超时时间为1s后
        itime.it_interval.tv_sec = 1; 
        itime.it_interval.tv_nsec = 0; //第一次超时后，每次超时的间隔时
        timerfd_settime(timerfd, 0, &itime, NULL);
        return timerfd;
    }
    //指针向后走一步
    void TickStep()
    {
        ++_tick %= _size;
        _wheel[_tick].clear();
    }
public:
    TimerWheel(int size,Eventloop* loop):_tick(0),_size(size),_wheel(size),_loop(loop)
                                         ,_timerfd(CreatTimer()),_timer_channel(_timerfd, loop) 
    {
        // 每隔1s写入一次timerfd已经启动CreateTimer，需要将监听timerfd的timer_channel加入到loop中，相当于每1s执行一次readcallback
        _timer_channel.SetReadCallback([this](){
            uint64_t times;
            int ret = read(_timerfd, &times, 8);
            if (ret < 0) {
                ERR_LOG("READ TIMEFD FAILED!");
                abort();
            }
            for(int i=0; i < times; i++)
            {
                TickStep();
            }
        });
        _timer_channel.EnableReadMonitor();
    }
    //添加定时任务
    //直接传一个timertask对象，不知道怎么构造那个shared_ptr的对象，这样对吗，智能指针管理的那个对象是不是在堆上？？？
    // void AddTimertask(TimerTask &tm)
    // {
    //     if(_timers.find(tm.Id()) != _timers.end()) { return; } 
    //     else 
    //     {
    //         std::shared_ptr<TimerTask> timertask(&tm);
    //         _timers.insert(std::make_pair(tm.Id(),timertask));
    //     }
    // }    
    //添加定时任务
    //这里直接传一个函数对象，避免上面的问题
    void AddTimertask(uint64_t id,uint32_t timeout,const TaskCallBack& cb); //timerwheel为临界资源，有并发问题，为什么？？？？？
    //刷新定时任务
    void RefreshTimertask(uint64_t id);
    //取消一个定时任务
    void CancelTimertask(uint64_t id);
    /*这个接口存在线程安全问题--这个接口实际上不能被外界使用者调用，只能在模块内，在对应的EventLoop线程内执行*/
    bool HasTimer(uint64_t id) 
    {
        auto it = _timers.find(id);
        if (it == _timers.end()) {
            return false;
        }
        return true;
    }
};
