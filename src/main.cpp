//
#include "reactor/epoll_reactor.h"
#include "thread_pool.h"
#include <iostream>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <csignal>
#include <memory>

bool g_running = true;

void signal_handler(int sig) {
    (void)sig;
    g_running = false;
    std::cout << "\n[Server] Shutdown signal received." << std::endl;
}

int main() {
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    int listen_fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
    if (listen_fd < 0) { perror("socket failed"); return 1; }

    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(8080);

    int opt = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    if (bind(listen_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("bind failed");
        close(listen_fd);
        return 1;
    }

    if (listen(listen_fd, SOMAXCONN) < 0) {
        perror("listen failed");
        close(listen_fd);
        return 1;
    }

    std::cout << "[Server] V2.1 SharedPtr + ThreadPool Version on 8080" << std::endl;

    try {
        // 使用智能指针管理线程池
        auto pool = std::make_shared<ThreadPool>(4);
        EpollReactor reactor(listen_fd);

        // 核心改动：conn 现在是一个 shared_ptr
        reactor.SetOnMessage([pool](std::shared_ptr<Connection> conn, const std::string& data) {
            // 生产者：将 shared_ptr 拷贝一份进 Lambda
            // 只要这个任务还在队列或正在运行，conn 指向的对象就不会被析构
            pool->AddTask([conn, data]() {
                // 打印当前线程 ID，验证多线程运行
                // std::cout << "[Thread " << std::this_thread::get_id() << "] Processing..." << std::endl;

                std::string response = 
                    "HTTP/1.1 200 OK\r\n"
                    "Content-Type: text/plain\r\n"
                    "Content-Length: " + std::to_string(data.size()) + "\r\n"
                    "\r\n" + data;
                
                conn->Send(response);
            });
        });

        reactor.Run();

    } catch (const std::exception& e) {
        std::cerr << "Fatal error: " << e.what() << std::endl;
    }

    if (listen_fd >= 0) close(listen_fd);
    return 0;
}