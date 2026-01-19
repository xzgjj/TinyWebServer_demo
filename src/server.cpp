
//

#include "server.h"
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdexcept>
#include <thread>
#include <iostream>
#include <cstring>
#include <cerrno>

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
    // 允许重用本地地址
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    // 允许重用端口 (Linux 3.9+)，解决连续重启时 bind failed 问题
    setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt));

    struct sockaddr_in addr{}; // 只保留这一个定义
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, ip.c_str(), &addr.sin_addr);

    if (bind(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        // 打印具体的 errno 错误信息，方便定位
        std::cerr << "[Fatal] bind failed on port " << port << ": " << strerror(errno) << std::endl;
        throw std::runtime_error("bind failed");
    }

    if (listen(fd, 4096) < 0) {
        throw std::runtime_error("listen failed");
    }

    return fd;
}

void Server::Start() {
    std::cout << "[Server] Event loop starting..." << std::endl;
    if (reactor_) {
        // Reactor::Run() 现在会响应 g_running 并在信号到达后退出
        reactor_->SetOnMessage(on_message_);
        reactor_->Run();
    }
    std::cout << "[Server] Event loop finished." << std::endl;
}

void Server::Stop() {
    std::cout << "[Server] Stopping..." << std::endl;
    if (reactor_) {
        reactor_->Stop();
    }
}