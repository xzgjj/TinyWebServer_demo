//
#include <iostream>
#include <csignal>
#include <memory>
#include "server.h"
#include "http_request.h"
#include "connection.h"

bool g_running = true;

void signal_handler(int sig) {
    (void)sig;
    g_running = false;
    std::cout << "\n[Server] Shutdown signal received." << std::endl;
}

int main() {
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    // 完全托管给 Server 类
    Server server("0.0.0.0", 8080);
    std::cout << "[Server] TinyWebServer V3 (FSM Protocol) on port 8080" << std::endl;

    server.SetOnMessage([](std::shared_ptr<Connection> conn, const std::string& /*data*/) {
        auto parser = conn->GetHttpParser();
        auto& buffer = conn->GetInputBuffer();

        // 状态机解析逻辑
        if (parser->Parse(buffer)) {
            std::string path = parser->GetPath();
            
            std::string body = "<html><body><h1>TinyWebServer V3</h1><p>Path: " + path + "</p></body></html>";
            std::string header = "HTTP/1.1 200 OK\r\n";
            header += "Content-Type: text/html\r\n";
            header += "Content-Length: " + std::to_string(body.size()) + "\r\n";
            header += "Connection: close\r\n\r\n";
            
            conn->Send(header + body);
            
            // 重置解析器并关闭连接（V3阶段暂不处理长连接复杂竞争）
            parser->Reset(); 
            //conn->Close();
        }
    });

    try {
        server.Start();
    } catch (const std::exception& e) {
        std::cerr << "Fatal error: " << e.what() << std::endl;
    }

    return 0;
}