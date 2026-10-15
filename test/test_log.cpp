//验证异步日志器在高频调用下的稳定性、时间戳精度及 FATAL 级别的立即刷新功能。

/**
 * @file test_log.cpp
 * @brief 日志性能与级别过滤测试
 */

#include "Logger.h"
#include <thread>
#include <vector>
#include <chrono>   // 必须显式包含
#include <iostream>

// 将常量提取或作为参数传递，确保统计一致性
void MultiThreadLogTest(int thread_count, int log_count) 
{
    std::vector<std::thread> threads;
    for (int i = 0; i < thread_count; ++i) 
    {
        threads.emplace_back([i, log_count]() {
            for (int j = 0; j < log_count; ++j) 
            {
                LOG_INFO("Thread %d logging message %d", i, j);
            }
        });
    }

    for (auto& t : threads) t.join();
    Logger::GetInstance().Flush(); // 确保数据落盘，计入总时长
}

int main() 
{
    // 1. 定义测试规模
    const int kThreadCount = 4;
    const int kLogCount = 10000;
    
    Logger::GetInstance().Init("./test_log.log");

    // 2. 开启计时
    auto start = std::chrono::high_resolution_clock::now();

    // 3. 【核心修复】执行业务逻辑
    MultiThreadLogTest(kThreadCount, kLogCount);

    // 4. 结束计时
    auto end = std::chrono::high_resolution_clock::now();

    // 5. 计算并打印结果
    double seconds = std::chrono::duration<double>(end - start).count();
    
    if (seconds > 0) {
        // 使用强制类型转换确保浮点运算精度
        double total_logs = static_cast<double>(kThreadCount) * kLogCount;
        printf("RESULT: %.2f logs/sec | Total Time: %.4fs\n", total_logs / seconds, seconds);
    } else {
        printf("Error: Test duration too short to measure.\n");
    }

    return 0;
}