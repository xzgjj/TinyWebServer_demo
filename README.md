

# TinyWebServer_demo


## 概述

TinyWebServer_v1 是一个基于 C++ 的轻量级高性能 Web 服务器，支持：
- 多连接并发处理
- 基于 Epoll 的事件驱动
- 非阻塞 I/O
- 简单的客户端生命周期管理
- 单元测试验证关键功能

## Features

- Single-thread epoll reactor
- Non-blocking socket read/write
- Output buffer + EPOLLOUT back-pressure
- RAII + C++17
- Safe connection lifecycle

## 服务器

                 ┌────────────┐
accept() ───────▶│ Connection │
                 └─────┬──────┘
                       │
                       ▼
               ┌─────────────────┐
               │ EpollReactor     │
               │  (IO only)       │
               └─────┬───────────┘
                     │ event
        ┌────────────┴────────────┐
        ▼                         ▼
┌──────────────┐        ┌────────────────┐
│ Read Handler │        │ Write Handler  │
│ (parse)      │        │ (flush buffer) │
└──────┬───────┘        └────────┬───────┘
       │                          │
       ▼                          ▼
┌────────────────────────────────────────┐
│            HTTP Layer                  │
│  Request Parse / Response Generate     │
└────────────────────────────────────────┘


## 架构与步骤

框架演进路线 /
v2   线程池 /
v3： 实现http /
v4   定时 和 异步日志 /
v5   mmp零拷贝  静态态资源系统和 / 火焰图
v6   线程池优化 参考项目  /

性能 /
QPS   平均延迟   并发支持 内存分配


##  实现

实现对应：

EpollReactor 类实现了 Epoll 事件驱动机制。

Connection 类封装了单个 TCP 连接的生命周期（状态管理、读写缓冲）。

main.cpp 使用 EpollReactor 注册监听 socket，接收客户端请求并调度事件。

2. 功能实现说明
2.1 多连接支持
支持同时处理多个客户端连接。


实现对应：

EpollReactor 内维护了 epoll_fd_ 和 std::unordered_map<int, Connection>。

当 epoll_wait 返回事件时，根据 fd 调用对应 Connection 的 HandleRead / HandleWrite。

2.2 非阻塞 I/O
所有客户端 socket 均为非阻塞模式，保证单线程事件循环不会被阻塞。





实现对应：

CreateListenSocket() 返回非阻塞的监听 socket。
Connection::Fd() 对应的 socket 也设置了 O_NONBLOCK。
TryFlushWriteBuffer() 实现非阻塞写，遇到 EAGAIN 返回等待下一轮事件。

2.3 客户端生命周期管理
每个客户端连接有状态：  - OPEN: 连接可读写  - CLOSED: 连接关闭


实现对应：
enum class ConnState { OPEN, CLOSED };
Connection::State() 返回当前状态
Connection::Close() 关闭 fd 并更新状态
EpollReactor::Run() 根据状态决定是否处理读写事件

2.4 事件驱动机制
使用 Epoll 进行事件通知：
- EPOLLIN -> 可读  - EPOLLOUT -> 可写   EPOLLERR / EPOLLHUP -> 异常处理


实现对应：
EpollReactor::UpdateInterest(Connection&) 注册/修改 epoll 事件
EpollReactor::Run() 循环调用 epoll_wait 并分发事件到对应 Connection

2.5 并发处理
单线程事件循环处理所有连接，可通过多线程扩展。


实现对应：
目前 v1 是单线程，依赖 epoll 高效轮询。
连接操作和缓冲区在 Connection 内部封装，避免全局共享数据竞争。

3. 架构与文件说明
src/
  main.cpp          // 服务器入口，初始化监听 socket 和 EpollReactor
  epoll_reactor.cpp // EpollReactor 实现
  connection.cpp    // Connection 类实现
  socket_utils.cpp  // socket 工具函数，如创建监听 socket
include/
  connection.h      // Connection 类声明
  epoll_reactor.h   // EpollReactor 类声明
tools.py            // 测试脚本




## Build

bash
mkdir -p build
cd build
cmake ..
make

make -j4

## build

python3 tools.py clean

mkdir -p build
cd build

配置 CMake
cmake -DCMAKE_BUILD_TYPE=Debug \
      -DCMAKE_CXX_FLAGS="-Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -O0 -g" \
      ..


cmake --build . --parallel

## debug
cmake -DCMAKE_BUILD_TYPE=Debug ..
在一个终端（左侧）启动服务器的 GDB 调试。 
gdb ./server 


第一步：清理环境   
python3 tools.py clean
第二步：全量编译并运行所有测试
python3 tools.py all
第三步：分析失败项 如果某个测试（例如 test_backpressure）失败，使用调试功能：
python3 tools.py debug --target test_backpressure

## 测试
make
完全自动化测试（推荐）

终端 1 (启动服务器):  ./server
终端 2 (运行测试脚本):

根目录运行测试
find . -type f -exec touch {} +

python3 tools.py build
python3 tools.py test

使用 GDB 调试特定测试
python3 tools.py debug --target test_backpressure

清理构建缓存
python3 tools.py clean

详细错误报告
python3 tools.py all

启动服务器：
./build/server

sudo kill -9 $(sudo lsof -t -i:8080)
# 强制杀死所有名为 server 的进程
pkill -9 server

/test_log_bench 8 200000