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
#include "reactor/socket_utils.h"
#include "connection.h"
#include "config/server_config.h"
#include "http/keep_alive_manager.h"

namespace tinywebserver {
class ServerConfig;
}

class Server {
public:
    Server(const std::string& ip, int port);
    explicit Server(const std::shared_ptr<tinywebserver::ServerConfig>& config);
    ~Server();
    void Start();
    void Stop();
    void Run();
    
    // 设置消息回调
    void SetOnMessage(Connection::MessageCallback cb) {
        on_message_ = std::move(cb);
    }

    // 获取 Keep-Alive 管理器
    tinywebserver::KeepAliveManager* GetKeepAliveManager() const {
        return keep_alive_manager_.get();
    }

    void SetupConnectionInLoop(std::shared_ptr<Connection> conn);
    void RemoveConnection(int fd);

private:

    Connection::MessageCallback on_message_;
    void HandleAccept(int listen_fd);
    void NewConnection(int fd);
    

    std::unique_ptr<EventLoop> main_loop_;  
    std::unique_ptr<EventLoopThreadPool> thread_pool_;
    int listen_fd_;

    std::mutex conn_mutex_;
    std::unordered_map<int, std::shared_ptr<Connection>> connections_;
    std::shared_ptr<tinywebserver::ServerConfig> config_;
    std::unique_ptr<tinywebserver::KeepAliveManager> keep_alive_manager_;
};


#endif
