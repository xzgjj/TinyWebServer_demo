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

bool g_running = true;

EpollReactor::EpollReactor(int listen_fd)
    : listen_fd_(listen_fd)
{
    epoll_fd_ = ::epoll_create1(0);
    if (epoll_fd_ < 0) throw std::runtime_error("epoll_create1 failed");

    epoll_event ev {};
    ev.events = EPOLLIN; 
    ev.data.fd = listen_fd_; // 这里的 listen_fd_ 现在应该是类似 3, 4 这样的小数字了
    
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

void EpollReactor::AcceptLoop()
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

        auto conn = std::make_unique<Connection>(client_fd);
        epoll_event ev {};
        ev.events = EPOLLIN; // 默认只监听读
        ev.data.fd = client_fd;

        if (::epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, client_fd, &ev) < 0)
        {
            ::close(client_fd);
            continue;
        }
        conns_[client_fd] = std::move(conn);
        std::cout << "[Server] New connection, fd: " << client_fd << std::endl;
    }
}

void EpollReactor::UpdateInterest(Connection& conn)
{
    epoll_event ev {};
    ev.data.fd = conn.Fd();
    
    // 基础事件：始终监听读，防止半关闭状态丢失
    ev.events = EPOLLIN; 

    // 关键：检查是否有待发送的数据
    if (conn.HasPendingWrite()) {
        ev.events |= EPOLLOUT; 
    }

    // 必须使用 MOD 模式更新已存在的 FD
    if (::epoll_ctl(epoll_fd_, EPOLL_CTL_MOD, conn.Fd(), &ev) < 0) {
        // 如果 MOD 失败，打印错误，方便调试
        perror("epoll_ctl MOD failed");
    }
}

void EpollReactor::Run()
{
    std::array<epoll_event, 64> events {};

    std::cout << "[Server] Reactor is running on 8080..." << std::endl;

    while (g_running)
    {
        int n = ::epoll_wait(epoll_fd_, events.data(),
                             static_cast<int>(events.size()), -1);

        if (n < 0) {
            if (errno == EINTR) continue;
            break;
        }

        for (int i = 0; i < n; ++i)
        {
            int fd = events[i].data.fd;

            // 1. 处理新连接请求
            if (fd == listen_fd_)
            {
                AcceptLoop();
                continue;
            }

            auto it = conns_.find(fd);
            if (it == conns_.end()) continue;

            Connection& conn = *it->second;

            // 2. 处理读事件 (EPOLLIN)
            if (events[i].events & EPOLLIN)
            {
                conn.HandleRead();
                
                // 【修改关键点】
                // 读完后，数据如果进了写缓冲区（Echo），
                // 必须调用 UpdateInterest 来开启 EPOLLOUT，否则写操作永远不会被触发。
                UpdateInterest(conn);
            }

            // 3. 处理写事件 (EPOLLOUT)
            // 只有当状态为 OPEN 且缓冲区有数据时才处理
            if (conn.State() == ConnState::OPEN && (events[i].events & EPOLLOUT))
            {
                conn.HandleWrite();
                
                // 【修改关键点】
                // 写完后再次更新，如果写缓冲区空了，UpdateInterest 会移除 EPOLLOUT，
                // 从而避免“忙轮询（Busy Loop）”。
                UpdateInterest(conn);
            }

            // 4. 清理连接
            // 如果在 HandleRead 中检测到 EOF(n=0)，状态会被设为 CLOSED
            if (conn.State() == ConnState::CLOSED)
            {
                std::cout << "[Server] Closing connection, fd: " << fd << std::endl;
                ::epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, fd, nullptr);
                conns_.erase(it);
            }
        }
    }
    std::cout << "Reactor stopped gracefully." << std::endl;
}