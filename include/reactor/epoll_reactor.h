

// epoll_reactor.h
#ifndef EPOLL_REACTOR_H
#define EPOLL_REACTOR_H

#include <sys/epoll.h>
#include <unordered_map>
#include <memory>
#include <mutex>
#include <string>
#include <functional>
#include "connection.h"
#include "timer_manager.h"

// 定义回调类型
using MessageCallback = std::function<void(std::shared_ptr<Connection>, const std::string&)>;

class EpollReactor {
public:
    EpollReactor();
    explicit EpollReactor(int listen_fd);
    ~EpollReactor();

    void Run();
    void Stop();
    
    // 修复：添加这些被 Server.cpp 或 EpollReactor.cpp 使用的成员函数
    void HandleRead(int fd);
    void HandleWrite(int fd);
    void AddConnection(int fd);
    void RemoveConnection(int fd);
    void HandleAccept(); 
    void SetOnMessage(MessageCallback cb) { on_message_ = cb; }
    void UpdateEvent(int fd, uint32_t events);

private:
    int epoll_fd_;
    int listen_fd_;
    bool running_;
    
    static const int MAX_EVENTS = 1024;
    struct epoll_event events_[MAX_EVENTS];

    std::mutex conn_mutex_;
    std::unordered_map<int, std::shared_ptr<Connection>> connections_;
    std::unique_ptr<TimerManager> timer_manager_;
    MessageCallback on_message_; // 存储业务回调
};

#endif