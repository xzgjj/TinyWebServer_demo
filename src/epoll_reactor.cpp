//

#include "reactor/epoll_reactor.h"
#include "connection.h"
#include <memory>
#include <stdexcept>

#include <sys/epoll.h>
#include <sys/socket.h>
#include <fcntl.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>
#include <array>
#include <vector>
#include <iostream>
#include <netinet/in.h>    // struct sockaddr_in (修复该报错的关键)
#include <arpa/inet.h>     // inet_ntoa 等地址转换（如果用到）


extern bool g_running; // 使用 main.cpp 中定义的全局变量

// 构造函数初始化定时器
EpollReactor::EpollReactor(int listen_fd) 
    : epoll_fd_(epoll_create1(0)), 
      listen_fd_(listen_fd), 
      running_(false), 
      timer_manager_(std::make_unique<TimerManager>()) {
    if (epoll_fd_ < 0) {
        perror("epoll_create1 failed");
        exit(EXIT_FAILURE);
    }
}

EpollReactor::~EpollReactor()
{
    if (epoll_fd_ >= 0) {
        ::close(epoll_fd_);
    }
}

// 辅助函数：设置文件描述符为非阻塞
static int SetNonBlocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1) return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

// src/epoll_reactor.cpp
void EpollReactor::HandleAccept() {
    struct sockaddr_in client_addr;
    socklen_t client_addr_len = sizeof(client_addr);
    
    // ET 模式核心：循环直到内核队列清空
    while (true) {
        int client_fd = accept(listen_fd_, (struct sockaddr*)&client_addr, &client_addr_len);
        if (client_fd == -1) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break; // 正常接完
            perror("accept failed");
            break;
        }

        SetNonBlocking(client_fd);
        auto conn = std::make_shared<Connection>(client_fd, this);
        {
            std::lock_guard<std::mutex> lock(conn_mutex_);
            connections_[client_fd] = conn;
        }

        // 绑定定时器
        std::weak_ptr<Connection> weak_conn = conn;
        timer_manager_->AddTimer(client_fd, 60000, [this, client_fd, weak_conn]() {
            if (auto shared_conn = weak_conn.lock()) {
                this->RemoveConnection(client_fd);
            }
        });

        // 注册 EPOLLET
        struct epoll_event ev;
        ev.data.fd = client_fd;
        ev.events = EPOLLIN | EPOLLET | EPOLLRDHUP;
        epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, client_fd, &ev);
    }
}


void EpollReactor::AddConnection(int fd) {
    
    auto conn = std::make_shared<Connection>(fd, this); 
    
    std::lock_guard<std::mutex> lock(conn_mutex_);
    connections_[fd] = conn;

    // 注册定时器：60秒不活跃自动关闭
    // 使用 weak_ptr 防止 Connection 和回调之间产生循环引用
    std::weak_ptr<Connection> weak_conn = conn;
    timer_manager_->AddTimer(fd, 60000, [this, fd, weak_conn]() {
        if (auto shared_conn = weak_conn.lock()) {
            std::cout << "[Timer] FD=" << fd << " idle timeout. Cleaning up..." << std::endl;
            this->RemoveConnection(fd);
        }
    });

    // 绑定 epoll 事件逻辑（略）...
}

void EpollReactor::RemoveConnection(int fd) {
    std::lock_guard<std::mutex> lock(conn_mutex_);
    auto it = connections_.find(fd);
    if (it != connections_.end()) {
        epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, fd, nullptr);
        connections_.erase(it);
        // 彻底清理定时器，防止残留任务重复触发
        timer_manager_->RemoveTimer(fd);
        std::cout << "[Reactor] FD=" << fd << " removed." << std::endl;
    }
}



void EpollReactor::Run() {
    running_ = true;
    
    // 1. 首先将监听 Socket 加入 epoll 实例
    struct epoll_event ev;
    ev.events = EPOLLIN | EPOLLET; // 边缘触发
    ev.data.fd = listen_fd_;
    if (epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, listen_fd_, &ev) == -1) {
        perror("epoll_ctl: listen_fd");
        return;
    }

    std::cout << "[Reactor] Event loop started." << std::endl;

    // 2. 主循环
    while (g_running && running_) {
        // 获取最近的定时器超时时间，避免 epoll_wait 永久阻塞
        int timeout = timer_manager_->GetNextTimeout();
        
        int n = epoll_wait(epoll_fd_, events_, MAX_EVENTS, timeout);
        
        if (n < 0) {
            if (errno == EINTR) continue; // 处理中断信号
            break;
        }

        for (int i = 0; i < n; ++i) {
            int fd = events_[i].data.fd;
            uint32_t revents = events_[i].events;

            if (fd == listen_fd_) {
                HandleAccept(); // 处理新客户端连接
            } else {
                // 处理已有连接的读写
                if (revents & (EPOLLIN | EPOLLPRI | EPOLLRDHUP)) {
                    HandleRead(fd);
                }
                if (revents & EPOLLOUT) {
                    HandleWrite(fd);
                }
                if (revents & (EPOLLERR | EPOLLHUP)) {
                    RemoveConnection(fd);
                }
            }
        }
        // 3. 每轮循环检查并处理过期定时器（踢掉超时客户端）
        timer_manager_->HandleExpiredTimers();
    }
    std::cout << "[Reactor] Event loop stopped." << std::endl;
}

void EpollReactor::Stop() {
    running_ = false;
}

void EpollReactor::HandleRead(int fd) {
    std::cout << "[Debug] HandleRead triggered for FD=" << fd << std::endl;
    std::shared_ptr<Connection> conn;
    {
        std::lock_guard<std::mutex> lock(conn_mutex_);
        if (connections_.count(fd)) {
            conn = connections_[fd];
        }
    }

    if (!conn) return;

    // 1. 调用 Connection 的读取逻辑。
    // 注意：在 ET 模式下，conn->Recv() 内部必须使用 while 循环直到返回 EAGAIN
    int bytes_read = conn->Recv(); 

    if (bytes_read > 0) {
        if (on_message_) {
            // 2. 触发回调。注意：这里不要在主线程 ClearReadBuffer
            // 此时数据已经追加到了 Connection 的 InputBuffer 中
            on_message_(conn, ""); 
        }
        // 3. 更新定时器，证明此连接依然活跃
        if (timer_manager_) {
            timer_manager_->AdjustTimer(fd, 30000); // 延长 30 秒寿命
        }
    } 
    else if (bytes_read == 0) {
        // 对端关闭 (FIN)
        std::cout << "[Reactor] Client closed connection, FD=" << fd << std::endl;
        RemoveConnection(fd);
    } 
    else {
        // 读取出错
        if (errno != EAGAIN && errno != EWOULDBLOCK) {
            std::cout << "[Reactor] Read error on FD=" << fd << " (errno: " << errno << ")" << std::endl;
            RemoveConnection(fd);
        }
    }
}


void EpollReactor::HandleWrite(int fd) {
    std::shared_ptr<Connection> conn;
    {
        std::lock_guard<std::mutex> lock(conn_mutex_);
        if (connections_.count(fd)) {
            conn = connections_[fd];
        }
    }

    if (!conn) return;

    // 1. 调用 Connection 的发送逻辑
    int bytes_sent = conn->Send();

    if (bytes_sent < 0) {
        if (errno != EAGAIN && errno != EWOULDBLOCK) {
            perror("HandleWrite failed");
            RemoveConnection(fd);
        }
    }
    
    // 2. 如果数据全部发送完毕，可以考虑取消关注 EPOLLOUT 事件，避免 busy loop
    // 这里根据你的 LT/ET 模式设计而定
}

void EpollReactor::UpdateEvent(int fd, uint32_t events) {
    struct epoll_event ev;
    ev.data.fd = fd;
    ev.events = events;
    if (epoll_ctl(epoll_fd_, EPOLL_CTL_MOD, fd, &ev) == -1) {
        perror("epoll_ctl MOD failed");
    }
}