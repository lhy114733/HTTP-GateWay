#pragma once
#include <condition_variable>
#include <errno.h>
#include <string.h>
#include <assert.h>
#include <mutex>
#include <thread>
#include <fcntl.h>
#include <unistd.h>
#include <sys/eventfd.h>

#include "../comm/util.hpp"
#include "channel.hpp"
#include "timerwheel.hpp"

//主要负责对epoll的封装，将要监听的事件写入内核并记录监听了哪些事件(unordered_map)
#define MAX_EVENTS 1024
class Poller 
{
private:
    int _epfd;
    struct epoll_event _evs[MAX_EVENTS]; //用于提升效率
    std::unordered_map<int,Channel*> _channels;


private:
    //在内核中更改指定channel监听的事件
    void Update(int op,Channel* channel)
    {
        //epoll_ctl(int epfd, int op, int fd, struct epoll_event *event)
        struct epoll_event event;
        event.events = channel->Event();
        event.data.fd = channel->Fd();
        int ret = epoll_ctl(_epfd,op,channel->Fd(),&event);
        if(ret < 0)
        {
            ERR_LOG("EPOLL_CTL ERR!");
            return;
        }
    }
public:
    Poller():_epfd(-1) 
    {
        _epfd = epoll_create(1);
    }
    //添加或更改_channels中要求监听的事件，并在内核中更改。   
    //难道是给channel用的？channel的Enable和Diable函数用的。应该是对的，Enable和Disable是channel事件写入unordered_map和内核对外的接口，也就是给connection用的，
    //poller对象提前创建一个，而一个个创建的是channel对象，创建完channel对象(指定一个poller对象)之后，直接使用Enable就写入unordered_map和内核中，并开始监听了

    //poller是那个数组和内核，channel是那个数组里的元素，应该是在有每个元素之后，把每个元素同时写到数组和内核中，直接操纵的是每个元素，
    //poller是对epoll(内核)和数组的封装，是用来简化对每个元素写入的操作的
    //封装的操作有epoll_create(构造函数)，epoll_ctl(Update、Remove)，epoll_wait(Poll)以及对应的在数组中增加修改删除的操作
    //channel直接调用上面这些封装好的接口，极大的简化了操作，
    //而channel本身也是一层封装，是对一个fd的要监听事件、就绪事件、事件触发回调函数的封装，这层封装也极大简化了操作
    //  根据上面来看，channel难道是poller的上层？？？毕竟poller更接近系统层，哈希表看成中间层
    void UpdateChannel(Channel* channel) 
    {
        auto it = _channels.find(channel->Fd());
        if(it == _channels.end())
        {
            _channels.insert(std::make_pair(channel->Fd(),channel));
            Update(EPOLL_CTL_ADD,channel);
            return;
        }
        it->second = channel;
        Update(EPOLL_CTL_MOD,channel);
    }
    //删除某个_channels事件，并在内核中删除
    void RemoveChannel(Channel* channel)
    {
        auto it = _channels.find(channel->Fd());
        if(it == _channels.end()) return;
        _channels.erase(it);
        Update(EPOLL_CTL_DEL,channel);
    }
    //内核中返回一次监听结果
    void Poll(std::vector<Channel*>& active)//用vector，不用传个数了，可以size()直接获得个数
    {//                    注意active是输出型参数，一定要加&，不然就是拷贝了！！永远是空
        //epoll_wait(int epfd, struct epoll_event *events, int maxevents, int timeout)
        int nfds = epoll_wait(_epfd,_evs,MAX_EVENTS,-1); //这里为啥是-1
        //这里如果不在对象里创建_evs的话，每次调用这个函数都要创建一次_evs[1024]，会降低效率
        if(nfds<0)
        {
            if(errno == EINTR) return;
            ERR_LOG("EPOLL_WAIT ERROR:%s\n",strerror(errno));
            abort();
        }
        //根据data.fd查找哈希表是否存在该channel，存在就更改该channel的revents，再把该channel插入到输出型参数active里
        for(int i = 0; i<nfds; i++)
        {
            auto it = _channels.find(_evs[i].data.fd);
            //if(it == _channels.end()) return;
            assert(it!=_channels.end());
            it->second->SetREvent(_evs[i].events);
            active.push_back(it->second);
        }
    }
};

class TimerWheel; // 这里有一个循环包含的问题
//事件循环
//监听事件->处理就绪事件->执行任务队列
class Eventloop
{
private:
    Poller _poller;
    using functor = std::function<void()>;
    std::mutex _mutex;
    std::vector<functor> _functors;
    std::thread::id _thread_id; //线程id,判断是否是自己线程

    int _eventfd;               //用来避免epoll阻塞而不能执行任务队列
    Channel _eventfd_channel;

    TimerWheel _timerwheel; // timerwheel在构造时就已经放到eventloop里了，已经启动读监控了
                            // 而且在构造时就已经创建了timerfd，启动了每秒向timerfd写一次，稳定触发读就绪，稳定推动时间论向后走
                            // 所以理解timerwheel时间论是通过timerfd的channel放到eventloop中的，才加入了时间轮功能
                            // timerfd本身没啥用，它的作用就是把timerwheel放到eventloop里

private:
    int CreateEventfd() 
    { 
        int ret = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK ); //为什么非阻塞????
        if(ret < 0)
        {
            ERR_LOG("CREATE EVENTFD ERR!"); 
            abort();
        }
        return ret;
    }
    void EventfdReadHandel()
    {
        uint64_t s = 0;
        ssize_t ret = read(_eventfd, &s, sizeof(s));
        if(ret < 0)
        {
            //EINTR -- 被信号打断；   EAGAIN -- 表示无数据可读
            if (errno == EINTR || errno == EAGAIN) {
                return;
            }
            ERR_LOG("READ EVENTFD ERR!"); 
            abort();
        }
    }
public:
    Eventloop():_thread_id(std::this_thread::get_id()),_eventfd(CreateEventfd()),
                _eventfd_channel(_eventfd,this),_timerwheel(60, this) // 时间论长度为60s
    {
        _eventfd_channel.SetReadCallback( std::bind(&Eventloop::EventfdReadHandel,this));
        _eventfd_channel.EnableReadMonitor();
    }
    bool HasTimer(uint64_t id)
    {
        return _timerwheel.HasTimer(id);
    }

    //对事件轮的操作函数，增加任务、刷新任务、取消任务
    void AddTimertask(uint64_t id, uint32_t timeout, const TaskCallBack &cb)  // 这回调什么时候设置？？？？？？？任务应该是非活跃连接销毁把
    {
        _timerwheel.AddTimertask(id, timeout, cb);
    }
    void CancelTimertask(uint64_t id)
    {
        _timerwheel.CancelTimertask(id);
    }
    void RefreshTimertask(uint64_t id)
    {
        _timerwheel.RefreshTimertask(id);
    }

    //在内核中修改删除监听事件
    void Remove(Channel* channel) { _poller.RemoveChannel(channel); }
    void Update(Channel* channel) { _poller.UpdateChannel(channel); }

    //监听事件->处理就绪事件->执行任务队列
    void Start()
    {
        while(1) // 理解eventloop是一个事件循环，poller可不是，每次循环都执行一次poller
        {
            //获取就绪事件
            std::vector<Channel*> active;
            _poller.Poll(active);
            //处理就绪事件
            for(auto &a : active) //为什么要加引用
            {
                a->HandleEvent(); // 调用每个channel的handleevent
            }
            //执行任务队列
            std::vector<functor> functors;
            {
                std::unique_lock<std::mutex> _lock(_mutex);
                functors.swap(_functors); 
            }
            for(auto &func : functors)
            {
                func();
            }
        }
    }

    //判断要执行任务是否在当前线程，在就执行，不在就加入任务队列
    void RunInLoop(const functor& func)
    {
        //在线程内直接执行
        if(_thread_id == std::this_thread::get_id()) { func(); }
        else  
        {
            QueueInLoop(func);
        }
    }

    //永远加入任务队列，不管当前在哪个线程。
    //专门给"回调里可能把自己销毁掉"的场景用：如果内联执行，销毁动作就发生在
    //Channel::HandleEvent 的调用栈里，回调返回后 HandleEvent 还在用 this -> use-after-free。
    //排队到 Start() 的任务处理阶段执行，那时事件处理的栈帧已经退干净了。
    void QueueInLoop(const functor& func)
    {
        std::unique_lock<std::mutex> _lock(_mutex);
        _functors.push_back(func);
        //eventfd唤醒epoll，防止阻塞
        uint64_t n = 1;
        ssize_t ret = write(_eventfd, &n, sizeof(n));
        if(ret < 0)
        {
            if (errno == EINTR) {
                return;
            }
            ERR_LOG("WRITE EVENTFD ERR!");
            abort();
        }
    }
};

inline void Channel::Remove() { _loop->Remove(this); }
inline void Channel::Update() { _loop->Update(this); }

inline void TimerWheel::AddTimertask(uint64_t id,uint32_t timeout,const TaskCallBack& cb) //timerwheel为临界资源，有并发问题，为什么？？？？？
{
    _loop->RunInLoop([this,id,timeout,cb]() //必须按值捕获：AddTimertask返回后 &cb 就悬垂了
    {
        if(_timers.find(id) != _timers.end()) { return; } 
        else 
        {
            std::shared_ptr<TimerTask> tm = std::make_shared<TimerTask>(id,timeout,cb);
            tm->SetRelease([this,id](){ // 为什么这里不能&this
                // 删除哈希表的索引的回调，也就是在任务释放后调用该函数删掉时间轮里的哈希表索引
                auto iter = _timers.find(id); // 为什么这里不用this->_timers也可以，语法就是这样吗？但是必须捕捉this???
                if(iter != _timers.end())
                {
                    _timers.erase(iter);
                }
            });
            _timers.insert(std::make_pair(id,tm));   //插入哈希表索引
            _wheel[(_tick + timeout) % _size].push_back(tm);//将任务对象放入事件轮
        }
    });
}
inline void TimerWheel::RefreshTimertask(uint64_t id)
{
    _loop->RunInLoop([this,id](){
        auto it = _timers.find(id);
        if(it == _timers.end()) { return; } 
        else 
        {
            std::shared_ptr<TimerTask> pt = it->second.lock();
            if (!pt) { _timers.erase(it); return; } //任务已到期销毁，索引是脏的
            _wheel[(_tick + pt->Timeoout()) % _size].push_back(pt); //push_back后，pt会被销毁吗
            //              pt 变量本身析构，但它管理的 TimerTask 对象被 _wheel 中的拷贝继续持有，不会销毁。
        }
    });
}
inline void TimerWheel::CancelTimertask(uint64_t id) 
{
    _loop->RunInLoop([this,id](){
        auto iter = _timers.find(id);
        if (iter == _timers.end()) { return; }
        std::shared_ptr<TimerTask> pt = iter->second.lock();
        if (pt) { pt->SetCanceled(); }
        //只打取消标记，不立刻删索引：索引留给任务对象析构时的_release回调去删（和参考实现一致）。
        //若在这里提前删掉，同一id重新Add会建出新任务，而旧任务析构时的_release
        //会把新任务的索引误删，导致新任务变成孤儿(Refresh/Cancel都失效)
    });
}


//eventloop整合了poller和timerwheel模块，就要把这两个模块的功能向上提供出来，提供给connection

// 先创建线程，再在线程里创建eventloop
class LoopThread 
{
private:
    Eventloop* _loop;
    std::thread _thread;
    std::mutex _mutex;
    std::condition_variable _cond;
private:
    void ThreadEntry()
    {
        Eventloop loop; // 为什么不new一个_loop
        {
            std::unique_lock<std::mutex> lock(_mutex); //加锁
            _loop = &loop; 
            _cond.notify_all();
        }
        _loop->Start(); // 也就是说，一创建好线程，一初始化完，就开始了开始一轮一轮的监听（启动了）
    }
public:
    LoopThread():_loop(nullptr), _thread(std::bind(&LoopThread::ThreadEntry,this))
    {}
    Eventloop *GetLoop() 
    {
        Eventloop *loop = NULL; // 感觉这里没啥必要啊，为什么不能直接返回_loop
        {
            std::unique_lock<std::mutex> lock(_mutex);// 加锁
            _cond.wait(lock, [&](){ return _loop != nullptr; });//loop为NULL就一直阻塞
            loop = _loop; // 感觉这里没啥必要啊，为什么不能直接返回_loop
        }
        return loop;
    }
};

// 从属线程池，只需要知道主循环(主线程)是哪个就行了，不开辟主线程，只开辟从属线程
class LoopThreadPool 
{
private:
    int _thread_count;
    int _next_idx;
    Eventloop* _baseloop;
    std::vector<LoopThread* > _threads;
    std::vector<Eventloop* > _loops;
public:
    LoopThreadPool(int thread_count, Eventloop* baseloop):_thread_count(thread_count), _next_idx(0), _baseloop(baseloop),
                                         _threads(thread_count),_loops(thread_count)
    {}
    void InitiaLoopThread() //初始化完了就启动了
    {
        for(int i = 0; i < _thread_count; i++)
        {
            _threads[i] = new LoopThread();  // 注意到push_back和[i]的区别，push_back直接加到最后一个了(可以扩容)，[i]是更改指定的(不会越界报错，是未知行为)！！！
            _loops[i] = _threads[i]->GetLoop(); // 一定是等new的LoopThread线程构造完Eventloop才能得到这个loop;
        }
    }

    // 获得下一个Eventloop
    Eventloop *NextLoop() 
    {
        if (_thread_count == 0) {
            return _baseloop;
        }
        _next_idx = (_next_idx + 1) % _thread_count;
        return _loops[_next_idx];
    }
};