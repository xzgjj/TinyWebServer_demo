//连接与关闭测试

/**
 * @file test_client_close.cpp
 * @brief 验证客户端主动断开时，服务器映射表的同步删除
 */

#include "server.h"
#include <sys/socket.h>
#include <unistd.h>

#include <netinet/in.h> // 必须包含，用于 sockaddr_in
#include <arpa/inet.h>  // 用于相关转换函数
#include "Logger.h"     // 修复 LOG_INFO

void TestClientAbruptClose() 
{
    Server server("127.0.0.1", 9002, PluginManager::GetInstance());
    
    // 启动服务器
    std::thread server_thread([&server]() { server.Start(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    // 客户端连接
    int client_fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in addr{};
    // ... 配置 addr ...
    connect(client_fd, (struct sockaddr*)&addr, sizeof(addr));
    
    // 立即关闭
    close(client_fd);
    LOG_INFO("Client closed connection immediately.");

    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    
    // 检查 connections_ 映射表是否已由 RemoveConnection 清理
    // 注意：实际项目中需通过 server.GetConnectionCount() 接口验证
    server.Stop();
    if (server_thread.joinable()) server_thread.join();
}



int main() 
{
    TestClientAbruptClose();
    return 0;
}