


// epoll_reactor.h
#pragma once
#include "connection.h"
#include <unordered_map>
#include <memory>
#include <array>
#include <functional>
#include <sys/epoll.h>

class EpollReactor {
public:
    // 定义回调类型
    using OnMessageCallback = std::function<void(std::shared_ptr<Connection>, const std::string&)>;

    explicit EpollReactor(int listen_fd);
    ~EpollReactor();

    // 设置回调函数
    void SetOnMessage(OnMessageCallback cb) { on_message_ = std::move(cb); }

    void Run();

private:
    // 处理新连接
    void HandleAccept();
    // 修改监听事件（由 Read 变为 Read|Write）
    void UpdateInterest(int fd, uint32_t events);
    // 移除连接
    void RemoveConnection(int fd);

    int listen_fd_;
    int epoll_fd_;
    
    // 核心存储：fd 映射到智能指针
    std::unordered_map<int, std::shared_ptr<Connection>> connections_;
    std::array<epoll_event, 64> events_;
    OnMessageCallback on_message_;
};