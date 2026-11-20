#include "channel.hpp"
#include "socket.hpp"
#include "eventloop.hpp"

class Acceptor 
{
private:
    Socket _socket;
    Eventloop* _loop;
    Channel _channel;

    using AcceptCallback = std::function<void(int)>;
    AcceptCallback _accept_callback;
private:
    // 改成static：不碰this->_socket，避免"构造_socket时调用会用到_socket的成员函数"(未定义行为)
    static int CreateServer(uint16_t port, std::string ip)
    {
        Socket sock; //因为Socket只有一个fd成员变量，只关心这个fd就行了，所以可以这样写为static
        if (!sock.CreateServer(port, ip, true)) return -1;
        return sock.Fd(); //Socket析构不会close，fd交给_socket接管
    }
    /*监听套接字的读事件回调处理函数---获取新连接，调用_accept_callback函数进行新连接处理*/
    void HandleRead() {
        int newfd = _socket.Accept();
        if (newfd < 0) {
            return ;
        }
        if (_accept_callback) _accept_callback(newfd);  // std::placeholders::_1在这里
    }
public:
    Acceptor(Eventloop* loop, uint16_t port, std::string ip):
            _socket(CreateServer(port, ip)), 
            _loop(loop), _channel(_socket.Fd(), _loop)
    {
        _channel.SetReadCallback(std::bind(&Acceptor::HandleRead, this));
    }
    void StartAccept() { _channel.EnableReadMonitor(); } // 启动读监控，相当于开始获得已经连接的描述符，叫accept好一点
    void SetAcceptCallback(const AcceptCallback& cb) { _accept_callback = cb; }
};