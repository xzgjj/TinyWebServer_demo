//
#ifndef SERVER_H
#define SERVER_H

#include <string>
#include <memory>
#include <vector>
#include <mutex>
#include <unordered_map>
#include "reactor/event_loop.h"
#include "reactor/event_loop_thread_pool.h"
#include "connection.h"

class Server {
public:
    Server(const std::string& ip, int port);
    ~Server();
    void Start();
    void Stop();
    
    // 设置消息回调
    void SetOnMessage(Connection::MessageCallback cb) { 
        on_message_ = std::move(cb); 
    }

    void SetupConnectionInLoop(std::shared_ptr<Connection> conn);
    void RemoveConnection(int fd);

private:
    void HandleAccept(int listen_fd);
    void NewConnection(int fd);
    int CreateListenFd(const std::string& ip, int port);

    int listen_fd_;
    std::unique_ptr<EventLoop> main_loop_;
    std::unique_ptr<EventLoopThreadPool> thread_pool_;
    Connection::MessageCallback on_message_;

    std::mutex conn_mutex_;
    std::unordered_map<int, std::shared_ptr<Connection>> connections_;
};

#endif
