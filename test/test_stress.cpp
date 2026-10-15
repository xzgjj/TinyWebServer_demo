//高并发压力测试


/**
 * @file test_stress.cpp
 * @brief Stress test for connection handling and backpressure.
 */

#include "server.h"
#include <vector>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <thread>
#include <cstring>        
#include <chrono>   // 必须包含计时库
#include <atomic>   // 建议包含原子操作
#include "Logger.h"       

/**
 * @brief 模拟高频率客户端请求并返回成功数
 */
int SimulateClients(uint16_t port, int connections)
{
    std::vector<int> fds;
    fds.reserve(static_cast<size_t>(connections));
    int success_count = 0;

    for (int i = 0; i < connections; ++i) 
    {
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) continue;

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

        if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) == 0) 
        {
            fds.push_back(fd);
            const char* msg = "GET /index.html HTTP/1.1\r\nHost: localhost\r\n\r\n";
            send(fd, msg, strlen(msg), 0);
            success_count++;
        }
        else 
        {
            close(fd); // 连接失败需关闭 fd
        }
    }

    LOG_INFO("Stress test: %d active connections established.", success_count);

    // 保持连接一段时间以便服务器处理
    std::this_thread::sleep_for(std::chrono::seconds(2));

    for (int fd : fds) 
    {
        close(fd);
    }
    return success_count;
}

int main()
{
    Logger::GetInstance().Init("./stress_test.log");

    // 1. 启动服务器
    std::thread server_thread([]() 
    {
        Server server("127.0.0.1", 9000, PluginManager::GetInstance());
        server.Start();
    });
    server_thread.detach();

    std::this_thread::sleep_for(std::chrono::seconds(1));

    // 2. 开始压力测试计时
    auto start = std::chrono::high_resolution_clock::now();
    
    int connections_to_run = 500;
    int total_requests = SimulateClients(9000, connections_to_run); // 获取实际成功数

    auto end = std::chrono::high_resolution_clock::now();
    
    // 3. 统计计算
    std::chrono::duration<double> diff = end - start;
    double duration = diff.count(); 
    double avg_latency = (total_requests > 0) ? (duration / total_requests) * 1000.0 : 0;

    // 4. 彻底修复：确保 printf 括号匹配、变量对应且有分号
    if (duration > 0) 
    {
        printf("RESULT: QPS: %.2f | Latency: %.2fms | Total: %d\n", 
               ((double)total_requests / duration), 
               avg_latency, 
               total_requests); 
    } // 这里是 if 的闭合

    return 0;
}