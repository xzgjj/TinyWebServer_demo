//多线程 Reactor 测试  ;验证 MainLoop 分发任务到 SubLoop 的线程安全性


/**
 * @file test_multithread_reactor.cpp
 * @brief Tests the One-Loop-Per-Thread model.
 */

#include "reactor/event_loop.h"
#include "reactor/event_loop_thread.h"
#include "Logger.h"
#include <vector>
#include <memory>
#include <set>
#include <mutex>

/**
 * @brief 验证不同 Loop 运行在不同线程
 */
void TestThreadDistribution()
{
    std::vector<std::unique_ptr<EventLoopThread>> threads;
    std::set<std::thread::id> thread_ids;
    std::mutex mtx;

    for (int i = 0; i < 4; ++i) 
    {
        threads.emplace_back(std::make_unique<EventLoopThread>());
        EventLoop* loop = threads.back()->StartLoop();
        
        loop->RunInLoop([&mtx, &thread_ids]() 
        {
            std::lock_guard<std::mutex> lock(mtx);
            thread_ids.insert(std::this_thread::get_id());
        });
    }

    // 等待异步任务执行
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    if (thread_ids.size() == 4) 
    {
        LOG_INFO("MultiThread Reactor Test: PASSED (4 distinct threads)");
    } 
    else 
    {
        LOG_ERROR("MultiThread Reactor Test: FAILED, expected 4 threads, got %zu", thread_ids.size());
    }
}

int main()
{
    Logger::GetInstance().Init("./test_reactor.log");
    TestThreadDistribution();
    return 0;
}