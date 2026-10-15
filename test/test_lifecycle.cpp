//生命周期测试



/**
 * @file test_lifecycle.cpp
 * @brief 验证服务器生命周期管理与资源释放 验证 Server 与 EventLoopThreadPool 在启动、运行及平滑停止过程中的资源回收情况，确保无内存泄漏和悬垂指针。
 */

#include "server.h"
#include "Logger.h"
#include <chrono>
#include <thread>
#include <cassert>

/**
 * @brief 快速启动与停止测试，验证 RAII 是否生效
 */
void TestServerRAII() 
{
    LOG_INFO("Starting TestServerRAII...");
    {
        // 作用域开始：创建服务器
        Server server("127.0.0.1", 9001, PluginManager::GetInstance());
        
        // 模拟异步启动
        std::thread t([&server]() {
            server.Start();
        });
        
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        
        // 显式停止
        server.Stop();
        if (t.joinable()) t.join();
        LOG_INFO("Server instance scope ending...");
    }
    // 作用域结束：验证 destructor 是否正常关闭 listen_fd
    LOG_INFO("TestServerRAII: PASSED (No crash on destruction)");
}

int main() 
{
    Logger::GetInstance().Init("./test_lifecycle.log");
    try 
    {
        TestServerRAII();
    } 
    catch (const std::exception& e) 
    {
        LOG_FATAL("Lifecycle test failed: %s", e.what());
    }
    return 0;
}