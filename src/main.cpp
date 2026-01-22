//
#include <iostream>
#include <csignal>
#include <memory>
#include <sys/stat.h>
#include <unistd.h>
#include "server.h"
#include "http_response.h"
#include "http_request.h"
#include "connection.h"
#include "Logger.h"

int main() {
    // 1. 确保环境收敛：创建资源目录
    struct stat st;
    if (stat("./www", &st) != 0) {
        mkdir("./www", 0755);
        // 创建默认首页以防 404 导致集成测试失败
        system("echo '<h1>TinyWebServer V3</h1>' > ./www/index.html");
    }

    Logger::GetInstance().Init("./tiny_server.log", LogLevel::LOG_LEVEL_DEBUG);
    Server server("0.0.0.0", 8080);

    server.SetOnMessage([](std::shared_ptr<Connection> conn, const std::string& /*data*/) {
        auto parser = conn->GetHttpParser();
        auto& buffer = conn->GetInputBuffer();

        // 核心修复：循环处理，直到缓冲区数据不足以构成一个完整 Header
        while (true) {
            size_t header_end = buffer.find("\r\n\r\n");
            if (header_end == std::string::npos) {
                break; // 数据不足，跳出等待下次 Read
            }

            if (parser->Parse(buffer)) {
                HttpResponse response;
                // 确保 Init 逻辑能正确处理路径
                response.Init("./www", parser->GetPath(), false); 
                response.MakeResponse();

                // 异步发送：Reactor 会处理发送队列
                conn->Send(response.GetHeaderString());
                if (response.HasFileBody()) {
                    conn->Send(response.GetFileBody()); 
                } else {
                    conn->Send(response.GetBodyString());
                }

                // --- 关键：精确消耗已解析的数据 ---
                // 注意：这里假设 Parse 仅处理了 Header，Body 逻辑需视业务而定
                // 在当前简单模型下，我们移除到 \r\n\r\n 之后
                buffer.erase(0, header_end + 4); 
                
                int status_code = response.GetCode();
                parser->Reset(); // 为下一次解析重置状态

                // 如果出错，则优雅关闭写端
                if (status_code >= 400) {
                    conn->Shutdown();
                    break;
                }
            } else {
                // 解析协议错误，清除坏数据并断开
                buffer.clear();
                conn->Shutdown();
                break;
            }
        }
    });

    LOG_INFO("Server starting on port 8080...");
    server.Start();
    server.Run();
    return 0;
}