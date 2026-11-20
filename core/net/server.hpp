#pragma once

// 伞形头（umbrella header）。
// 旧代码里 #include "server.hpp" 的写法继续可用，实际实现已经拆到下面各文件：
//
//   util.hpp      日志宏
//   buffer.hpp    用户态读写缓冲
//   socket.hpp    fd 生命周期 / bind / listen / accept
//   channel.hpp   一个 fd 的事件集合 + 回调分发
//   poller.hpp    epoll 封装 + 哈希表
//   eventloop.hpp 事件循环 + 任务队列 + eventfd 唤醒
//
// 新代码建议直接 include 用到的那一个，别再依赖这个大杂烩，编译会快很多。
