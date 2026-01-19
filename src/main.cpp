//
#include <iostream>
#include <csignal>
#include <memory>
#include "server.h"
#include "http_request.h"
#include "connection.h"
#include "Logger.h"

bool g_running = true;

void signal_handler(int sig) {
    (void)sig;
    g_running = false;
    std::cout << "\n[Server] Shutdown signal received." << std::endl;
}

int main() 
{
    Logger::GetInstance().Init("./tiny_server.log");
    LOG_INFO("Server starting on port %d...", 8080);

    signal(SIGINT, signal_handler);

    // 完全托管给 Server 类
    Server server("0.0.0.0", 8080);
    std::cout << "[Server] TinyWebServer V3 (FSM Protocol) on port 8080" << std::endl;
    
    server.SetOnMessage([](std::shared_ptr<Connection> conn, const std::string& data) {
        LOG_INFO("Message received from FD: %d, data size: %zu", conn->GetFd(), data.size());
        
        // 通过新的公共方法获取 HTTP 解析器和输入缓冲区
        auto parser = conn->GetHttpParser();
        auto& buffer = conn->GetInputBuffer();

        // 如果缓冲区为空，直接返回
        if (buffer.empty()) return;

        // 执行解析
        if (parser->Parse(buffer)) {
            std::string path = parser->GetPath();
            
            // 构造简单的 HTTP 响应
            std::string body = "<html><body><h1>TinyWebServer V3</h1><p>Path: " + path + "</p></body></html>";
            std::string header = "HTTP/1.1 200 OK\r\n"
                                 "Content-Type: text/html\r\n"
                                 "Content-Length: " + std::to_string(body.size()) + "\r\n"
                                 "Connection: close\r\n\r\n";
            
            // 发送数据
            conn->Send(header + body);

            // 解析并响应成功后，清空缓冲区和重置解析器状态
            conn->ClearReadBuffer(); 
            parser->Reset();  // 假设 HttpRequest 有 Reset 方法
        } else {
            // 如果 Parse 返回 false，说明数据不全，保留 buffer，等待下一次 HandleRead 拼接
            LOG_DEBUG("Incomplete HTTP request, waiting for more data from FD: %d", conn->GetFd());
        }
    });
    
    std::cout << "[Server] TinyWebServer V3 is running..." << std::endl;
    server.Start();
    
    // 等待信号
    while (g_running) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    
    server.Stop();
    LOG_INFO("Server stopped gracefully.");
    
    return 0;
}