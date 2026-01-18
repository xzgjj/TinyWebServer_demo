


// epoll_reactor.h
#pragma once
#include "connection.h"   // ConnState 和 Connection 已经包含
#include <map>
#include <memory>
#include <array>
#include <sys/epoll.h>

class EpollReactor {
public:
    explicit EpollReactor(int listen_fd);
    ~EpollReactor();

    void AcceptLoop();
    void UpdateInterest(Connection& conn);
    void Run();

private:
    int listen_fd_;
    int epoll_fd_;
    std::map<int, std::unique_ptr<Connection>> conns_;
    std::array<epoll_event, 64> events_;
};
