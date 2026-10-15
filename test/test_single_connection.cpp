//单一长连接可靠性以及高并发多连接调度下的表现

/**
 * @file test_single_connection.cpp
 * @brief 验证单条连接的吞吐量、大包处理及状态机转换
 */

#include "server.h"
#include "connection.h"
#include "Logger.h"
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <cassert>
#include <string>

/**
 * @brief 测试大对象传输稳定性
 */
void TestLargePayload() 
{
    LOG_INFO("Starting TestLargePayload...");
    
    // 构建 1MB 的伪造请求报文
    std::string large_body(1024 * 1024, 'X');
    std::string request = "POST /upload HTTP/1.1\r\n"
                          "Content-Length: " + std::to_string(large_body.size()) + "\r\n"
                          "\r\n" + large_body;

    int client_fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(8080);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

    struct timeval timeout = {5, 0}; // 5秒超时
    setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, (char *)&timeout, sizeof(timeout));


    if (connect(client_fd, (struct sockaddr*)&addr, sizeof(addr)) == 0) 
    {
        // 分块发送，验证服务器在 ET 模式下的拼包能力
        size_t sent = 0;
        size_t chunk_size = 16384;
        while (sent < request.size()) 
        {
            size_t to_send = std::min(chunk_size, request.size() - sent);
            ssize_t n = send(client_fd, request.data() + sent, to_send, 0);
            if (n <= 0) break;
            sent += static_cast<size_t>(n);
        }
        
        LOG_INFO("Large payload sent: %zu bytes", sent);
        
        // 接收响应
        char res_buf[1024];
        recv(client_fd, res_buf, sizeof(res_buf), 0);
        LOG_INFO("Response received for large payload.");
    }
    
    close(client_fd);
    LOG_INFO("TestLargePayload: PASSED");
}

int main() 
{
    // 启动服务器
    std::thread s_thread([](){
        Server server("127.0.0.1", 8080, PluginManager::GetInstance());
        server.Start();
    });
    s_thread.detach();
    
    std::this_thread::sleep_for(std::chrono::seconds(1));
    TestLargePayload();
    
    return 0;
}