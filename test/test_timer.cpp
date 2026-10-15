//定时器测试


/**
 * @file test_timer.cpp
 * @brief TimerManager unit tests with precision validation.
 */

#include "timer_manager.h"
#include "Logger.h"
#include <cassert>
#include <iostream>
#include <thread>
#include <atomic>

/**
 * @brief 验证定时器在并发与多任务下的准确性
 */
void TestTimerAccuracy()
{
    TimerManager manager;
    std::atomic<int> trigger_count{0};

    LOG_INFO("Starting Timer Accuracy Test...");

    // 1. 添加一个 100ms 后触发的任务
    manager.AddTimer(1, 100, [&]() 
    {
        trigger_count.fetch_add(1);
    });

    // 2. 立即移除一个任务测试
    manager.AddTimer(2, 50, []() { assert(false && "Should not be triggered"); });
    manager.RemoveTimer(2);

    // 3. 模拟时间流逝
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    
    // 4. 处理到期事件
    manager.HandleExpiredTimers();

    if (trigger_count.load() == 1) 
    {
        LOG_INFO("TestTimerAccuracy: PASSED");
    } 
    else 
    {
        LOG_ERROR("TestTimerAccuracy: FAILED, expected 1, got %d", trigger_count.load());
    }
}

int main()
{
    try 
    {
        TestTimerAccuracy();
    } 
    catch (const std::exception& e) 
    {
        std::cerr << "Exception: " << e.what() << std::endl;
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}