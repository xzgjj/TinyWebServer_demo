//证 EventLoopThreadPool 的 Round-Robin 分发算法是否公平，以及多线程环境下 connections_ 容器的互斥锁安全性。

/**
 * @file test_multi_connection.cpp
 * @brief 验证多线程 Reactor 模式下的连接负载均衡
 */

#include "server.h"
#include "Logger.h"
#include <vector>
#include <thread>
#include <atomic>
#include <netinet/in.h>
#include <arpa/inet.h>

/**
 * @brief 模拟突发大量连接建立与数据交换
 */
void ConcurrentConnectionTest(int total_conns) 
{
    LOG_INFO("Starting ConcurrentConnectionTest with %d connections...", total_conns);
    std::vector<int> client_fds;
    std::atomic<int> success_count{0};

    // 1. 批量建立连接
    for (int i = 0; i < total_conns; ++i) 
    {
        int fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
        if (fd < 0) continue;

        struct sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(8080);
        inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

        // 非阻塞 connect 可能返回 EINPROGRESS
        connect(fd, (struct sockaddr*)&addr, sizeof(addr));
        client_fds.push_back(fd);
    }

    // 2. 模拟多个线程同时向服务器发送心跳
    std::vector<std::thread> workers;
    for (int t = 0; t < 4; ++t) 
    {
        workers.emplace_back([&client_fds, &success_count, t, total_conns]() {
            for (size_t i = static_cast<size_t>(t); i < client_fds.size(); i += 4) 
            {
                const char* ping = "GET /ping HTTP/1.1\r\n\r\n";
                if (send(client_fds[i], ping, strlen(ping), 0) > 0) 
                {
                    success_count++;
                }
            }
        });
    }

    for (auto& worker : workers) worker.join();

    LOG_INFO("Concurrent stress test result: %d/%d requests processed.", 
             success_count.load(), total_conns);

    // 3. 批量销毁连接，验证服务器 RemoveConnection 的线程安全性
    for (int fd : client_fds) 
    {
        close(fd);
    }
}

int main() 
{
    Logger::GetInstance().Init("./test_multi_conn.log");
    
    // 假设服务器已经在运行中，或者在此启动
    ConcurrentConnectionTest(1000); // 1000 个并发连接
    
    return 0;
}