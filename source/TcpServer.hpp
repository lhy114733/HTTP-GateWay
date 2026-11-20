#include "connection.hpp"
#include "acceptor.hpp"
#include <memory>


class TcpServer 
{
private:
    uint64_t _next_id;
    bool _enable_inactive_release;
    int _timeout;  // 啥时候初始化了？？？

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
        _baseloop.AddTimertask(_next_id, delay, task);
    }
    void HandleNewConnection(int fd) //新连接到来怎么处理
    {
        _next_id++;
        std::shared_ptr<Connection> conn(new Connection(_next_id, fd, _pool.NextLoop()));
        conn->SetMessageCallback(_message_callback);
        conn->SetClosedCallback(_closed_callback);
        conn->SetConnectedCallback(_connected_callback);
        conn->SetAnyEventCallback(_event_callback);
        conn->SetSrvClosedCallback(std::bind(&TcpServer::RemoveConnection, this, std::placeholders::_1)); // 绑定conn关闭服务器回调函数
        if (_enable_inactive_release) conn->EnableInactiveRelease(_timeout);//启动非活跃超时销毁
        conn->Established();//就绪初始化
        _conns.insert(std::make_pair(_next_id, conn));
    }
    // void RemoveConnection(int fd); //为什么这里int fd编译会报错，按理来说std::placeholders::_1又识别不了类型，原因:bind完了类型是void(int)，
    // 和SrvCloseCallBack回调函数要求的void(std::shared_ptr<Connection>)类型不匹配了。所以bind是成功的，只是bind完了的函数类型和要求的不匹配了
    void RemoveConnectionForBind(const std::shared_ptr<Connection> &conn)  //用来移除服务器内部管理信息
    {
        int id = conn->Id();
        auto it = _conns.find(id);
        if (it != _conns.end()) {
            _conns.erase(it);
        }
    }
    //从管理Connection的_conns中移除连接信息
    void RemoveConnection(const std::shared_ptr<Connection> &conn) {
        _baseloop.RunInLoop(std::bind(&TcpServer::RemoveConnectionForBind, this, conn));
    }
public:
    TcpServer(uint16_t port, int thread_count):
                             _next_id(0), _port(port), _enable_inactive_release(false), 
                             _acceptor(&_baseloop, _port, "0.0.0.0"),
                             _pool(thread_count, &_baseloop),_timeout(0)
    {
        _acceptor.SetAcceptCallback(std::bind(&TcpServer::HandleNewConnection,this,std::placeholders::_1)); //绑定收到新连接的回调处理函数
        // std::placeholders::_1表明在调用时才传值，占位符的数量决定了调用时传入参数的数量
        // std::bind的一个作用减少函数调用时要传入参数的个数，减少参数个数靠的是"把某些参数直接绑定成固定值"。占位符不减少参数个数，保持个数
        _acceptor.StartAccept(); //启动读监听开始获得连接
        _pool.InitiaLoopThread();
    }
    // 回调函数设置
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
    void Start() { _baseloop.Start(); }
};