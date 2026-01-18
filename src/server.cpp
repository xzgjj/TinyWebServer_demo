
//

#include "server.h"
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdexcept>
#include <thread>

Server::Server(const std::string& ip, int port) {
    listen_fd_ = CreateListenFd(ip, port);
    // 使用 CPU 核心数的两倍
    unsigned int n_threads = std::thread::hardware_concurrency();
    thread_pool_ = std::make_shared<ThreadPool>(n_threads > 0 ? n_threads * 2 : 8);
    reactor_ = std::make_unique<EpollReactor>(listen_fd_);
}


Server::~Server() {
    if (listen_fd_ >= 0) ::close(listen_fd_);
}

int Server::CreateListenFd(const std::string& ip, int port) {
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
    if (fd < 0) throw std::runtime_error("socket failed");

    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, ip.c_str(), &addr.sin_addr);

    if (bind(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0)
        throw std::runtime_error("bind failed");

    if (listen(fd, 4096) < 0)
        throw std::runtime_error("listen failed");

    return fd;
}

void Server::Start() {
    // 将线程池绑定到 Reactor 的消息分发中
    // 这样当有消息产生时，Reactor 会把任务丢进线程池
    auto pool = thread_pool_;
    auto msg_cb = on_message_;
    
    reactor_->SetOnMessage([pool, msg_cb](std::shared_ptr<Connection> conn, const std::string& data) {
        pool->AddTask([conn, data, msg_cb]() {
            if (msg_cb) msg_cb(conn, data);
        });
    });

    // 进入 Epoll 循环
    reactor_->Run();
}