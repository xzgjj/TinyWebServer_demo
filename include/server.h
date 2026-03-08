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
#include "reactor/multi_listen_socket.h"
#include "reactor/batch_io_handler.h"
#include "connection.h"
#include "config/server_config.h"
#include "http/keep_alive_manager.h"

namespace tinywebserver {
class ServerConfig;
}

class Server {
public:
    // SO_REUSEPORT 配置选项
    struct SOReusePortOptions {
        bool enabled;
        size_t num_listen_sockets; // 0表示等于线程数

        SOReusePortOptions() : enabled(false), num_listen_sockets(0) {}
    };

    Server(const std::string& ip, int port,
           const SOReusePortOptions& reuseport_opts = SOReusePortOptions{});
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

    // 服务器参数
    std::string ip_;
    int port_;
    int backlog_;

    std::mutex conn_mutex_;
    std::unordered_map<int, std::shared_ptr<Connection>> connections_;
    std::shared_ptr<tinywebserver::ServerConfig> config_;
    std::unique_ptr<tinywebserver::KeepAliveManager> keep_alive_manager_;

    // SO_REUSEPORT 相关成员
    SOReusePortOptions reuseport_opts_;
    std::unique_ptr<MultiListenSocket> multi_listen_socket_;

    // SO_REUSEPORT 模式相关方法
    void SetupSOReusePortMode();
    void SetupTraditionalMode();
    void HandleAcceptInSubReactor(int listen_fd, EventLoop* sub_loop);
};


#endif
