//

#ifndef SERVER_H
#define SERVER_H

#include <string>
#include <memory>
#include <functional>
#include "reactor/epoll_reactor.h"
#include "thread_pool.h"
#include "connection.h"

class Server {
public:
    // 构造函数：指定监听 IP 和端口
    Server(const std::string& ip, int port);
    ~Server();

    // 禁止拷贝
    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;

    // 设置业务回调：当收到完整数据时调用
    void SetOnMessage(Connection::MessageCallback cb) {
        on_message_ = std::move(cb);
    }

    // 启动服务器（进入阻塞循环）
    void Start();  
    void Stop();// 停止服务器

    std::shared_ptr<ThreadPool> GetThreadPool() { return thread_pool_; }

private:
    // 初始化监听 Socket
    int CreateListenFd(const std::string& ip, int port);
    std::shared_ptr<ThreadPool> thread_pool_;
 
    int listen_fd_;
    std::unique_ptr<EpollReactor> reactor_;   
    Connection::MessageCallback on_message_;
};

#endif