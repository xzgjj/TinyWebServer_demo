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
    while (g_running) {
        int n = ::epoll_wait(epoll_fd_, events_.data(), (int)events_.size(), 1000);
        if (n < 0) {
            if (errno == EINTR) continue;
            break;
        }

        for (int i = 0; i < n; ++i) {
            int fd = events_[i].data.fd;
            uint32_t revents = events_[i].events;

            if (fd == listen_fd_) {
                HandleAccept(); continue;
            }

            std::shared_ptr<Connection> conn;
            {
                auto it = connections_.find(fd);
                if (it != connections_.end()) conn = it->second;
            }
            if (!conn) continue;

            // 1. 处理异常
            if (revents & (EPOLLERR | EPOLLHUP)) {
                conn->Close();
            } else {
                // 2. 处理读
                if (revents & (EPOLLIN | EPOLLRDHUP)) {
                    conn->HandleRead(on_message_);
                }
                // 3. 处理写
                if (conn->State() == ConnState::OPEN && (revents & EPOLLOUT)) {
                    conn->HandleWrite();
                }
            }

            // 4. 清理或更新
            if (conn->State() == ConnState::CLOSED) {
                ::epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, fd, nullptr);
                connections_.erase(fd);
            } else {
                uint32_t new_ev = EPOLLIN | EPOLLET | EPOLLRDHUP;
                if (conn->HasPendingWrite()) new_ev |= EPOLLOUT;
                
                epoll_event ev{};
                ev.events = new_ev;
                ev.data.fd = fd;
                ::epoll_ctl(epoll_fd_, EPOLL_CTL_MOD, fd, &ev);
            }
        }
    }
}