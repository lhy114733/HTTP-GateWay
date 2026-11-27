#pragma once
#include "../../comm/util.hpp"

#define DEFAULT_LISTEN_NUM 10
class Socket
{
  private:
    int _sockfd; 

  public:
    Socket(int sockfd = -1) : _sockfd(sockfd) {}
    int Fd() { return _sockfd; }
    bool Create()
    {
        _sockfd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, IPPROTO_TCP);
        if (_sockfd < 0)
        {
            ERR_LOG("SOCKET CREATE ERR!");
            return false;
        }
        return true;
    }
    bool Bind(uint16_t port, std::string ip)
    {
        struct sockaddr_in addr;
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        addr.sin_addr.s_addr = inet_addr(ip.c_str()); // arpa/inet.h
        socklen_t len = sizeof(addr);
        // bind(_sockfd, const struct sockaddr *addr, socklen_t len)
        int ret = bind(_sockfd, (const struct sockaddr *)&addr, len);
        if (ret < 0)
        {
            ERR_LOG("SOCKET BIND ERR!");
            return false;
        }
        return true;
    }
    bool Listen()
    {
        int ret = listen(_sockfd, DEFAULT_LISTEN_NUM);
        if (ret < 0)
        {
            ERR_LOG("SOCKET LISTEN ERR");
            return false;
        }
        return true;
    }
    bool Connect(uint16_t port, std::string ip)
    {
        struct sockaddr_in addr;
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port); // 必须转网络字节序，否则 8888 会被当成 47138
        addr.sin_addr.s_addr = inet_addr(ip.c_str()); // arpa/inet.h
        socklen_t len = sizeof(addr);
        // conect(_sockfd, const struct sockaddr *addr, socklen_t len)
        int ret = connect(_sockfd, (const struct sockaddr *)&addr, len);
        if (ret < 0)
        {
            // ERR_LOG("SOCKET CONNECT ERR!");
            // 非阻塞 connect：EINPROGRESS/EALREADY 表示还在进行，EINTR 表示被信号打断但连接仍会异步继续，
            // EISCONN 表示已经连上。这几种都不算失败，交给 EPOLLOUT + SO_ERROR 判定最终结果。
            if(errno == EINPROGRESS || errno == EALREADY || errno == EINTR || errno == EISCONN)
            {
                return true;
            }
            return false;
        }
        return true;
    }
    int Accept()
    {
        // accept(int fd, struct sockaddr *__restrict addr, socklen_t *__restrict addr_len)
        int newfd = accept(_sockfd, NULL, NULL);
        if (newfd < 0)
        {
            ERR_LOG("SOCKET ACCEPT ERR!");
            return -1;
        }
        SetNonBlock(newfd); //新连接也必须非阻塞，否则发送缓冲区满时send会卡死整个loop线程
        return newfd;
    }

    // 返回值语义和 recv() 保持一致：>0 实际字节数；==0 对端关闭(EOF)；<0 失败
    // EAGAIN/EWOULDBLOCK/EINTR 表示"暂时没数据"，不在这里归一化，由调用方看 errno 判断。
    // 注意：recv 返回 0(EOF) 时内核不会写 errno，所以绝不能用 errno 去判断 EOF
    ssize_t Recv(void *buf, size_t len, int flag = 0)
    {
        // ssize_t recv(int sockfd, void *buf, size_t len, int flag);
        ssize_t ret = ::recv(_sockfd, buf, len, flag);
        if (ret < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
        {
            ERR_LOG("SOCKET RECV FAILED!! errno=%d", errno);
        }
        return ret; //>0 数据长度；==0 对端关闭；<0 看errno
    }
     //发送数据
    ssize_t Send(const void *buf, size_t len, int flag = 0) 
    {
        // ssize_t send(int sockfd, void *data, size_t len, int flag);
        // 对端已关闭时内核会发SIGPIPE，默认动作是杀进程，必须用MSG_NOSIGNAL屏蔽
        ssize_t ret = ::send(_sockfd, buf, len, flag | MSG_NOSIGNAL);
        if (ret < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
                return 0;//暂时发不出去，调用方保持写监听即可
            }
            ERR_LOG("SOCKET SEND FAILED!! errno=%d", errno);
            return -1;
        }
        return ret;//实际发送的数据长度
    }
    void Close()
    {
        if (_sockfd != -1)
        {
            close(_sockfd);
            _sockfd = -1;
        }
    }
    // 交出 fd 的所有权：成功 connect 后由 Connection 接管，Socket 自己不再 close。
    // 没有这个接口，Connector 析构时要么泄漏 fd，要么把已经交给 Connection 的 fd 关掉。
    int Release()
    {
        int fd = _sockfd;
        _sockfd = -1;
        return fd;
    }
    // 把指定fd设置为非阻塞（accept出来的新连接也要用）
    static void SetNonBlock(int fd)
    {
        int flag = fcntl(fd, F_GETFL, 0);
        fcntl(fd, F_SETFL, flag | O_NONBLOCK);
    }
    // 设置为非阻塞
    void NonBlock()
    {
        SetNonBlock(_sockfd);
    }
    // 设置套接字选项---开启地址端口重用
    void ReuseAddress()
    {
        // int setsockopt(int fd, int leve, int optname, void *val, int vallen)
        int val = 1;
        setsockopt(_sockfd, SOL_SOCKET, SO_REUSEADDR, (void *)&val, sizeof(int));
        val = 1;
        setsockopt(_sockfd, SOL_SOCKET, SO_REUSEPORT, (void *)&val, sizeof(int));
    }

    bool CreateServer(uint16_t port, std::string ip, bool block_flag)
    {
        // 1.创建套接字 2.绑定端口 3.开启监听 4.设置非阻塞 5.启动端口重用
        if (Create() == false)
            return false;
        ReuseAddress();
        if (Bind(port, ip) == false)
            return false;
        if (Listen() == false)
            return false;
        if (block_flag)
            NonBlock();
        return true;
    }
    bool CreateClient(uint16_t port, std::string ip)
    {
        // 1.创建套接字 2.设置非阻塞 3.连接服务端
        if (Create() == false)
            return false;
        NonBlock();
        if (Connect(port, ip) == false) // 非阻塞式的connect只调用一次就好了，连接已经在进行中了
        {
            Close(); // connect 立即失败时把 fd 关掉，否则 Connector 侧很难判断所有权
            return false;
        }
        return true;
    }
    ~Socket() {}
};
