#pragma once
#include "eventloop.hpp"
#include "buffer.hpp"
#include "socket.hpp"
#include "channel.hpp"
#include <string>
#include <utility>
#include <unistd.h>

// 链接模块class Connection;
//DISCONECTED -- 连接关闭状态；   CONNECTING -- 连接建立成功-待处理状态
//CONNECTED -- 连接建立完成，各种设置已完成，可以通信的状态；  DISCONNECTING -- 待关闭状态
typedef enum { DISCONNECTED, CONNECTING, CONNECTED, DISCONNECTING}ConnStatus;
// 一个对象可不是建出来就有shared_ptr的，用了shared_ptr才有shared_ptr和weak_ptr
class Connection : public std::enable_shared_from_this<Connection> //继承的这个类，原理：内部生成一个weak_ptr
{
private:
    uint64_t _conn_id;
    ConnStatus _status;   // 连接状态
    bool _enable_inactive_release;
    Buffer _in_buffer; //读缓冲区
    Buffer _out_buffer; //写缓冲区
    bool _read_paused = false; // 用于暂停读，是用来控制的，本身不一定所处的连接属于背压状态才会被调用
    bool _output_aborted = false;
    bool _output_pressured = false;
    size_t _output_low = 0;
    size_t _output_high = 0;
    size_t _output_limit = 0;
    std::function<void(bool)> _output_pressure_callback;

    int _sockfd; 
    Socket _socket;
    Eventloop* _loop;
    Channel _channel;

    using ConnectedCallback = std::function<void(const std::shared_ptr<Connection>&)>;
    using MessageCallback = std::function<void(const std::shared_ptr<Connection>&, Buffer *)>;
    using ClosedCallback = std::function<void(const std::shared_ptr<Connection>&)>;
    using AnyEventCallback = std::function<void(const std::shared_ptr<Connection>&)>;
    ConnectedCallback _connected_callback;
    MessageCallback _message_callback;
    ClosedCallback _closed_callback;
    AnyEventCallback _event_callback;
    /*组件内的连接关闭回调--组件内设置的，因为服务器组件内会把所有的连接管理起来，一旦某个连接要关闭*/
    /*就应该从管理的地方移除掉自己的信息*/
    ClosedCallback _server_closed_callback;
private:
    // 这些Handle调用的回调全是用户设置的回调，感觉业务处理就是一层一层回调堆出来的
    void HandleRead()
    {
        if (_read_paused || _output_aborted || _status == DISCONNECTED) return;

        // 1. 读取socket读取就绪数据到_in_buffer
        char buf[65536];
        ssize_t ret = _socket.Recv(buf, 65535);
        if (ret == 0)
        {
            // 对端关闭(EOF)：水平触发下 epoll 会一直报可读，必须主动释放，否则100%CPU空转
            return Release();
        }
        if(ret < 0)
        {
            // 非阻塞下EAGAIN表示"这次没数据了"，不是错误
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
                return;
            }
            // 出错了，直接关闭，但是有可能还有未发送数据，先shutdown
            return Shutdown();
        }
        _in_buffer.Write(buf, ret); //Write内部会按需扩容，外层不用再判断空间
        if(_in_buffer.AbleReadSpace() > 0 && _message_callback)
        {
            // 2. message回调函数进行业务处理
            _message_callback(shared_from_this(), &_in_buffer);
        }
    }
    void HandleWrite()
    {
        if (_output_aborted || _status == DISCONNECTED) return;
        if (_read_paused && _channel.HasHangup())
        {
            // 挂断后留到恢复读取时取完对端已发送的数据。
            _channel.DisableWriteMonitor();
            return;
        }

        ssize_t ret = _socket.Send(_out_buffer.ReadPositon(), _out_buffer.AbleReadSpace());
        // 发送出错了，数据发不出去了，直接release()
        if(ret < 0 ) { return Release(); }
        _out_buffer.MoveReadIdx(ret);
        // 积压可能变小，改变积压
        UpdateOutputPressure(); 
        // 第一次开启写监听是第一次调Send的时候，必须直到发送完才结束写监听，要一直发送的。等到输出缓冲区里有数据了再打开写监听。
        // 而读监听是一直打开的
        // 理解，读数据是一有数据发来就要读并且处理，所以一直要都监听；
        // 而写监听只在Send时才开启，只有Send时才能把要发的数据放入_out_buffer，而不是一接收到消息处理完就必须放到_out_buffer，所以发送完了就关闭了
        if(_out_buffer.AbleReadSpace() == 0)
        {
            _channel.DisableWriteMonitor();
            // 如果要已经是半关闭状态，说明没有新数据到来，发送缓冲区里没东西就直接关闭连接了
            // 数据已发完直接调用真正的断开连接
            if(_status == DISCONNECTING) { Release(); }
        }
    }
    void HandleClose()
    {
        if (_output_aborted || _status == DISCONNECTED) return;
        if (_read_paused)
        {
            _channel.DisableWriteMonitor();
            return;
        }

        if(_in_buffer.AbleReadSpace() > 0 && _message_callback)
        {
            _message_callback(shared_from_this(), &_in_buffer);
        }
        Release(); // 如果用户处理完_in_buffer里的还要Send怎么办，这不是就发不出去了吗？？？连接已经关了，本来就发不出去了
    }
    void HandleError()
    {
        HandleClose();
    }
    void HandleEvent()
    {
        if (_enable_inactive_release == true)  {  _loop->RefreshTimertask(_conn_id); } //任意事件触发都刷新，如果有的话
        if (_event_callback)  {  _event_callback(shared_from_this()); }
    }

    // 更新积压
    void UpdateOutputPressure()
    {
        if (_output_limit == 0) return;

        const auto pending = _out_buffer.AbleReadSpace();
        const bool pressured = _output_pressured ? pending > _output_low : pending >= _output_high;
        if (pressured == _output_pressured) return;

        _output_pressured = pressured;
        // 进入背压状态回调
        if (_output_pressure_callback) _output_pressure_callback(pressured);
    }

    void SendInLoop(const char *data, size_t len)
    {
        if (_status == DISCONNECTED || _output_aborted) return;

        // _out_buffer带发送数据大于_output_limit，设置为_output_aborted，关闭连接
        const auto pending = _out_buffer.AbleReadSpace();
        if (_output_limit != 0 && (pending > _output_limit || len > _output_limit - pending))
        {
            _output_aborted = true;
            Release();
            return;
        }

        _out_buffer.Write(data, len);
        if (!_channel.WriteAble()) _channel.EnableWriteMonitor();
        // 向_out_buffer里写入，积压变大，更新积压
        UpdateOutputPressure();
    }

    void EnableInactiveReleaseForBind(int sec) {
        //1. 将判断标志 _enable_inactive_release 置为true
        _enable_inactive_release = true;
        //2. 如果当前定时销毁任务已经存在，那就刷新延迟一下即可
        if (_loop->HasTimer(_conn_id)) {
            return _loop->RefreshTimertask(_conn_id);
        }
        //3. 如果不存在定时销毁任务，则新增
        _loop->AddTimertask(_conn_id, sec, std::bind(&Connection::Release, this));  
        // 增加非活跃定时销毁任务，添加的是如果非活跃就release，为什么是release不是shutdown？？？这时候肯定没有要处理的数据了吗？？？。什么时候刷新任务？？？？
    }

public:
    Connection(uint64_t conn_id, int sockfd, Eventloop* loop, bool enable_inactive_release = true):
                                _conn_id(conn_id), _loop(loop), _sockfd(sockfd), _socket(sockfd), _status(CONNECTING), 
                                _channel(_sockfd, loop),_enable_inactive_release(enable_inactive_release)
    {
        // 设置channel的事件触发回调函数
        _channel.SetCloseCallback(std::bind(&Connection::HandleClose, this));
        _channel.SetEventCallback(std::bind(&Connection::HandleEvent, this));
        _channel.SetReadCallback(std::bind(&Connection::HandleRead, this));
        _channel.SetWriteCallback(std::bind(&Connection::HandleWrite, this));
        _channel.SetErrorCallback(std::bind(&Connection::HandleError, this));
    }
    int Id() { return _conn_id; }
    Eventloop *GetLoop() { return _loop; }

    // 设置水位，以及背压状态回调函数
    // 在所属 loop 上设置，回调可能立即报告当前连接仍有积压。
    void SetOutputWatermarks(size_t low, size_t high, size_t limit, std::function<void(bool)> callback)
    {
        assert(_loop->IsInLoopThread());
        assert(low < high && high < limit);
        _output_low = low;
        _output_high = high;
        _output_limit = limit;
        _output_pressure_callback = std::move(callback);

        const auto pending = _out_buffer.AbleReadSpace();
        // 根据_out_buffer的可读空间，改变背压状态
        _output_pressured = _output_pressured ? pending > low : pending >= high;
        if (_output_pressured && _output_pressure_callback) _output_pressure_callback(true);
    }

    bool OutputAborted() const { return _output_aborted; }

    // 启动或关闭paused
    void SetReadPaused(bool paused)
    {
        assert(_loop->IsInLoopThread());
        if (_status == DISCONNECTED || _read_paused == paused) return;

        _read_paused = paused;
        if (_status == CONNECTING) return;
        if (paused) _channel.DisableReadMonitor();
        else _channel.EnableReadMonitor();
    }

    // 回调函数
    void SetConnectedCallback(const ConnectedCallback&cb) { _connected_callback = cb; }
    void SetMessageCallback(const MessageCallback&cb) { _message_callback = cb; }
    void SetClosedCallback(const ClosedCallback&cb) { _closed_callback = cb; }
    void SetAnyEventCallback(const AnyEventCallback&cb) { _event_callback = cb; }
    void SetSrvClosedCallback(const ClosedCallback&cb) { _server_closed_callback = cb; }

    // 半连接后的处理
    // 1. 修改连接状态；  2. 启动读事件监控；  3. 调用回调函数(connected_callback,由用户设置)
    void Established() 
    {
        _loop->RunInLoop([this](){
            assert(_status == CONNECTING);//当前的状态必须一定是上层的半连接状态
            _status = CONNECTED;//当前函数执行完毕，则连接进入已完成连接状态
            // 一旦启动读事件监控就有可能会立即触发读事件，如果这时候启动了非活跃连接销毁
            // 先启动读监控再调回调（参考实现的顺序）：回调里如果立刻Send或Release，
            // 读监控已经就位，不会出现"写监控先挂上、读监控后补"的中间态
            if (!_read_paused) _channel.EnableReadMonitor();
            if (_connected_callback) _connected_callback(shared_from_this());
            // 这个回调里面如果写conn->Send()，等于在这里直接调Send(),因为类内知道自己的this指针，省略写了
        });
    }

    // this指针的精妙之处，理解这里省略的this指针，最终都会被编译器填上的。
    // _connected_callback(shared_from_this())是**手动传了this指针**，
    // 编译器懂就行
    void Send(const char *data, size_t len)
    {
        if (len == 0) return;
        if (_loop->IsInLoopThread())
        {
            SendInLoop(data, len);
            return;
        }

        // 跨线程投递时，任务持有字节和连接，直到所属 loop 处理。
        auto self = shared_from_this();
        _loop->QueueInLoop([self, bytes = std::string(data, len)]() {
            self->SendInLoop(bytes.data(), bytes.size());
        });
    }
    void Shutdown()
    {
        _loop->RunInLoop([this](){
            if (_output_aborted || _status == DISCONNECTED) return;
            _status = DISCONNECTING;
            if(_in_buffer.AbleReadSpace() > 0)
            {
                if (_message_callback) _message_callback(shared_from_this(), &_in_buffer);
            }
            if(_out_buffer.AbleReadSpace() > 0) // 输出缓冲区里还有数据，要发出去，可以发出去吗？？不可写怎么办？？？
            {
                if (_channel.WriteAble() == false) {
                    _channel.EnableWriteMonitor();
                }
            }
            if (_out_buffer.AbleReadSpace() == 0) //如果不等于0就不关了吗？？？ 
            {
                Release(); // 这里release也要runinloop，可能会造成问题吗？？
            }
        });
    }
    void Release()
    {
        //必须排队执行，不能内联：这条链路最后会 _conns.erase() 删掉最后一个 shared_ptr，
        //若在本线程内联执行，对象会在 Channel::HandleEvent 的调用栈里当场析构，
        //回到 HandleEvent 继续读 _revents / 调 _event_callback 就是 use-after-free。
        //捕获 self：客户端场景下 TcpClient 可能在 closed 回调后立刻释放 _conn，
        //不能让排队中的 lambda 继续拿着裸 this。
        auto self = shared_from_this(); //对于服务端这一行没用
        _loop->QueueInLoop([self](){
            if (self->_status == DISCONNECTED) return;
            //1. 修改连接状态，将其置为DISCONNECTED
            self->_status = DISCONNECTED;
            //2. 移除连接的事件监控
            self->_channel.Remove();
            //3. 关闭描述符
            self->_socket.Close();
            //4. 如果当前定时器队列中还有定时销毁任务，则取消任务
            if (self->_loop->HasTimer(self->_conn_id)) self->CancelInactiveRelease();
            //5. 调用关闭回调函数，避免先移除服务器管理的连接信息导致Connection被释放，再去处理会出错，因此先调用用户的回调函数
            if (self->_closed_callback) self->_closed_callback(self);
            //移除服务器内部管理的连接信息
            if (self->_server_closed_callback) self->_server_closed_callback(self);
        });
    }
    // 这个非活跃和活跃的函数看不懂
    void EnableInactiveRelease(int sec) // 理解poller和timerwheel是分离的，在poller里的不一定在timerwheel里，把定时任务加到timerwheel就是启动了非活跃销毁
    {
        _loop->RunInLoop(std::bind(&Connection::EnableInactiveReleaseForBind, this, sec));
    }
    void CancelInactiveRelease()
    {
        _loop->RunInLoop([this](){
            _enable_inactive_release = false;
            if (_loop->HasTimer(_conn_id)) { 
                _loop->CancelTimertask(_conn_id);  // 取消非活跃定时任务
            }
        });
    }
    ~Connection()
    {}
};

// Send(),Shutdown(),Release(),EnableInactiveRelease(),CancelInactiveRelease()应该是给用户用于设置回调函数的，
// 剩下的都是给TcpServer用的(回调设置,就绪初始化,启动非活跃连接销毁)，集中在HandleNewConnection