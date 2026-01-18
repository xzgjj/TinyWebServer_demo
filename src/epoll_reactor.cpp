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

extern bool g_running; // 使用 main.cpp 中定义的全局变量

EpollReactor::EpollReactor(int listen_fd)
    : listen_fd_(listen_fd)
{
    epoll_fd_ = ::epoll_create1(0);
    if (epoll_fd_ < 0) throw std::runtime_error("epoll_create1 failed");

    epoll_event ev {};
    ev.events = EPOLLIN; 
    ev.data.fd = listen_fd_; 
    
    if (::epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, listen_fd_, &ev) < 0) {
        throw std::runtime_error("epoll_ctl ADD listen_fd failed");
    }
} 

EpollReactor::~EpollReactor()
{
    if (epoll_fd_ >= 0) {
        ::close(epoll_fd_);
    }
}

// 处理新连接请求
void EpollReactor::HandleAccept()
{
    while (true)
    {
        // 使用 accept4 配合 SOCK_NONBLOCK 确保新连接是非阻塞的
        int client_fd = ::accept4(listen_fd_, nullptr, nullptr, SOCK_NONBLOCK);
        if (client_fd < 0)
        {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            break;
        }

        // 核心改动：使用 shared_ptr 管理
        auto conn = std::make_shared<Connection>(client_fd);
        
        epoll_event ev {};
        ev.events = EPOLLIN | EPOLLET; // V2 建议使用边缘触发 (ET) 配合非阻塞 IO
        ev.data.fd = client_fd;

        if (::epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, client_fd, &ev) < 0)
        {
            ::close(client_fd);
            continue;
        }

        // 存入 connections_ (unordered_map)
        connections_[client_fd] = std::move(conn);
        std::cout << "[Reactor] New connection, fd: " << client_fd << std::endl;
    }
}



void EpollReactor::Run() {
    struct epoll_event events[1024];
    while (true) {
        int n = epoll_wait(epoll_fd_, events, 1024, -1);
        for (int i = 0; i < n; ++i) {
            int fd = events[i].data.fd;

            if (fd == listen_fd_) {
                // 处理新连接逻辑 (Accept)
                HandleAccept();
            } else {
                auto conn = connections_[fd];
                if (!conn) continue;

                // 1. 处理可读事件
                if (events[i].events & EPOLLIN) {
                    conn->HandleRead(on_message_);
                }

                // 2. 处理可写事件 (当之前 Send 没发完时触发)
                if (events[i].events & EPOLLOUT) {
                    conn->HandleWrite();
                }

                // 3. 动态更新 Epoll 关注点 (核心优化)
                // 如果应用层缓冲区还有数据没发完，则必须关注 EPOLLOUT
                struct epoll_event ev;
                ev.data.fd = fd;
                ev.events = EPOLLIN | EPOLLET; // 默认边缘触发读
                if (conn->HasPendingWrite()) {
                    ev.events |= EPOLLOUT; // 缓冲区有数，增加写监听
                }

                if (conn->State() == ConnState::CLOSED) {
                    connections_.erase(fd);
                    epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, fd, nullptr);
                } else {
                    epoll_ctl(epoll_fd_, EPOLL_CTL_MOD, fd, &ev);
                }
            }
        }
    }
}