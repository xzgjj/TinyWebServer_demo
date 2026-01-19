
//

// 添加缺失的头文件
#include "server.h"
#include "connection.h"
#include "Logger.h"

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <cstring>
#include <cerrno>
#include <functional>

// ... 原有代码保持不变 ...


Server::Server(const std::string& ip, int port)
    : main_loop_(new EventLoop()),
      thread_pool_(new EventLoopThreadPool(main_loop_.get())),
      listen_fd_(CreateListenFd(ip, port)) {
    
    // 配置 Acceptor 的 Channel
    // 注意：这里直接操作 main_loop，因为它只负责 accept
    main_loop_->SetAcceptCallback(std::bind(&Server::HandleAccept, this, listen_fd_));
    main_loop_->UpdateEvent(listen_fd_, EPOLLIN | EPOLLET);
    
    LOG_INFO("Server started on %s:%d", ip.c_str(), port);
}

Server::~Server() {
    Stop();
    close(listen_fd_);
}

void Server::Start() {
    // 1. 启动线程池 (SubLoops)
    thread_pool_->SetThreadNum(4); // 建议设置为 CPU 核心数
    thread_pool_->Start();
    
    // 2. 启动主循环 (MainLoop)
    main_loop_->Loop();
}

void Server::Stop() {
    main_loop_->Stop();
    thread_pool_->Stop(); // 需在 ThreadPool 中实现 Stop
}



void Server::HandleAccept(int listen_fd) {
    struct sockaddr_in client_addr;
    socklen_t client_addr_len = sizeof(client_addr);
    
    // 循环 accept 处理并发连接（ET模式下必须读完）
    while (true) {
        int conn_fd = accept4(listen_fd, (struct sockaddr*)&client_addr, &client_addr_len, 
                             SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (conn_fd == -1) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break; // 处理完毕
            } else if (errno == EINTR) {
                continue;
            } else {
                LOG_ERROR("Accept error");
                break;
            }
        }
        
        // 【核心重构 V5】：获取一个 SubLoop，而不是使用当前的 MainLoop
        EventLoop* io_loop = thread_pool_->GetNextLoop();
        
        // 创建连接对象
        // 注意：连接归属于 io_loop，但目前我们在 MainLoop 线程中
        auto conn = std::make_shared<Connection>(conn_fd, io_loop);
        conn->SetMessageCallback(on_message_);
        conn->SetCloseCallback(std::bind(&Server::RemoveConnection, this, std::placeholders::_1));

        {
            // 加锁保护 connections_ 映射表（因为 RemoveConnection 可能在其他线程触发）
            std::lock_guard<std::mutex> lock(conn_mutex_);
            connections_[conn_fd] = conn;
        }

        LOG_INFO("New connection fd=%d assigned to loop thread %s", 
                 conn_fd, io_loop->GetThreadIdString().c_str());

        // 【关键】：将“连接建立”的任务派发到 io_loop 线程执行
        // 这样保证 Connection 的 Channel 操作完全在 IO 线程内
        io_loop->RunInLoop([conn]() {
            conn->ConnectEstablished();
        });
    }
}

void Server::RemoveConnection(int fd) {
    // 注意：此函数可能被 SubLoop 线程调用
    std::lock_guard<std::mutex> lock(conn_mutex_);
    size_t n = connections_.erase(fd);
    if (n == 1) {
        LOG_INFO("Connection fd=%d removed", fd);
    }
}

int Server::CreateListenFd(const std::string& ip, int port) {
    // (保持原有的 socket/bind/listen 逻辑不变)
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    // ... 省略 bind/listen 的常规代码 ...
    // 务必设置 SO_REUSEADDR
    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    
    struct sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = inet_addr(ip.c_str());
    
    bind(fd, (struct sockaddr*)&addr, sizeof(addr));
    listen(fd, 2048);
    return fd;
}