//
#include "reactor/epoll_reactor.h"
#include <iostream>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <csignal>

void signal_handler(int sig) {
    // 假设你有办法让 reactor 停止，或者直接用全局变量
    extern bool g_running; 
    g_running = false;
}


int main() {

    signal(SIGINT, signal_handler); // 捕获 Ctrl+C
    signal(SIGTERM, signal_handler); // 捕获终止信号
    // 1. 创建监听 socket
    int listen_fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
    if (listen_fd < 0) {
        perror("socket failed");
        return 1;
    }

    // 2. 绑定端口
    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(8080); // 这里才是放 8080 的地方

    // 设置地址复用，防止重启服务器时报 Address already in use
    int opt = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    if (bind(listen_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("bind failed");
        return 1;
    }

    if (listen(listen_fd, SOMAXCONN) < 0) {
        perror("listen failed");
        return 1;
    }

    try {
        // 3. 传入真正的 listen_fd (而不是 8080)
        EpollReactor reactor(listen_fd); 
        reactor.Run(); 
    } catch (const std::exception& e) {
        std::cerr << "Fatal error: " << e.what() << std::endl;
        return 1;
    }

    return 0;
}