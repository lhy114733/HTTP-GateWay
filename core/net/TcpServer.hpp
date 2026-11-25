#include "connection.hpp"
#include "acceptor.hpp"
#include <memory>

// 有一个主循环线程和一个从属循环线程池，把两个东西结合到一起了
class TcpServer 
{
private:
    uint64_t _next_id;
    bool _enable_inactive_release;
    int _timeout;  // 超时链接销毁时间

    Eventloop _baseloop; //主loop在主线程
    uint16_t _port;      //端口
    Acceptor _acceptor;  //acceptor在主loop

    LoopThreadPool _pool; //从属线程池和从属loop，从属loop一定要有一个主loop，所以要初始化主loop
    std::unordered_map<uint64_t, std::shared_ptr<Connection>> _conns; // 管理所有的连接

    using Functor = std::function<void()>;
    using ConnectedCallback = std::function<void(const std::shared_ptr<Connection>&)>;
    using MessageCallback = std::function<void(const std::shared_ptr<Connection>&, Buffer *)>;
    using ClosedCallback = std::function<void(const std::shared_ptr<Connection>&)>;
    using AnyEventCallback = std::function<void(const std::shared_ptr<Connection>&)>;
    ConnectedCallback _connected_callback;
    MessageCallback _message_callback;
    ClosedCallback _closed_callback;
    AnyEventCallback _event_callback;
private:
    void RunAfterForBind(const Functor &task, int delay) 
    {
        _next_id++;
        _baseloop.AddTimertask(_next_id, delay, task); //超时任务全加到baseloop主线程中了
    }
    void HandleNewConnection(int fd) // 最终给HandleRead回调
    {
        // 1.创建一个新Connection 2.设置新Connection的回调函数(连接初始化、信息处理、关闭、任意事件、服务器回调) 
        // 3.启动非活跃连接销毁 4.连接就绪初始化(打开读监控及其他) 5.把收到的新连接插入TcpServer的conns中管理 
        _next_id++;
        std::shared_ptr<Connection> conn(new Connection(_next_id, fd, _pool.NextLoop()));
        conn->SetConnectedCallback(_connected_callback);
        conn->SetMessageCallback(_message_callback);
        conn->SetClosedCallback(_closed_callback);
        conn->SetAnyEventCallback(_event_callback);
        conn->SetSrvClosedCallback(std::bind(&TcpServer::RemoveConnectionInSrvConns, this, std::placeholders::_1)); // 绑定conn关闭服务器回调函数
        if (_enable_inactive_release) conn->EnableInactiveRelease(_timeout);//启动非活跃超时销毁
        conn->Established();//就绪初始化
        // Acceptor收到的所有分发给Eventloop线程的连接全部都存到TcpServer主线程这里，管理所有线程的信息
        _conns.insert(std::make_pair(_next_id, conn));
    }
    void RemoveConnectionInSrvConnsForBind(const std::shared_ptr<Connection> &conn)  
    {
        int id = conn->Id();
        auto it = _conns.find(id);
        if (it != _conns.end()) {
            _conns.erase(it);
        }
    }
    // 相当于系统给NewConnection设置的回调函数，连接关闭时从管理TcpServer管理的_conns中移除该连接，_conns中有所有线程连接的信息
    void RemoveConnectionInSrvConns(const std::shared_ptr<Connection> &conn) 
    {
        _baseloop.RunInLoop(std::bind(&TcpServer::RemoveConnectionInSrvConnsForBind, this, conn));
    }
public:
    TcpServer(uint16_t port, int thread_count):
                             _next_id(0), _port(port), _enable_inactive_release(false), 
                             _acceptor(&_baseloop, _port, "0.0.0.0"),
                             _pool(thread_count, &_baseloop),_timeout(0)
    {
        _acceptor.SetAcceptCallback(std::bind(&TcpServer::HandleNewConnection,this,std::placeholders::_1)); // 就是设置Acceptor的读回调函数
        _acceptor.StartAccept(); //启动Acceptor读监听，在baseloop事件循环Start()开启后就可以获得新连接
        _pool.InitiaLoopThread(); //从属loop初始化完就全部启动事件循环了Start()
        //先初始化从属线程还是先启动accept？
    }
    // 服务端收到客户端信息的回调函数设置，都是给NewConnection设置的回调函数，通过HandleNewConnection设置给
    void SetConnectedCallback(const ConnectedCallback&cb) { _connected_callback = cb; }
    void SetMessageCallback(const MessageCallback&cb) { _message_callback = cb; }
    void SetClosedCallback(const ClosedCallback&cb) { _closed_callback = cb; }
    void SetAnyEventCallback(const AnyEventCallback&cb) { _event_callback = cb; }
    // 启动超时销毁
    void EnableInactiveRelease(int timeout) { _timeout = timeout; _enable_inactive_release = true; } //在确定要启动超时销毁时在设置timeout更合理
    //用于添加一个定时任务
    void RunAfter(const Functor &task, int delay) {
        _baseloop.RunInLoop(std::bind(&TcpServer::RunAfterForBind, this, task, delay));
    }
    void Start() { _baseloop.Start(); } // 启动baseloop的事件循环，不断获得就绪事件并处理，以及处理任务队列（Acceptor读事件监听在这之前开启）
};


// TcpServer的使用 (围绕NewConnection理解)
// 1.创建TcpServer对象(从属循环线程已创建并开始事件循环Start()) 
// 2.设置回调函数(连接就绪，信息处理，关闭，任意)，给connction用的回调 
// 3.设置是否开启超时连接销毁 (2、3是给Acceptor创建NewConnection时用的)
// 4.Start启动baseloop
// 5.任意时间可添加定时任务
// 注意：一定要理解所有回调函数的参数都有PtrConnection&(一个引用)，最终调用时传入的是shared_form_this()，
// 理解使用这个参数conn->Send()就是发送给这个连接对应的客户端，因为回调被调用时传入的是shared_form_this()，是当前连接，
// 理解这种写法，类似于HttpServer的那个Writer！！！