//

#ifndef THREAD_POOL_H
#define THREAD_POOL_H

#include <vector>
#include <queue>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <functional>

/**
 * @brief 线程池类：负责管理工作线程并执行异步任务
 * 实现原理：生产者-消费者模型
 */
class ThreadPool {
public:
    // 构造函数：启动 thread_count 个工作线程
    explicit ThreadPool(size_t thread_count = 8);
    
    // 析构函数：通知所有线程退出并回收资源
    ~ThreadPool();

    // 严禁拷贝：线程池作为底层资源管理类，不应被复制
    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    /**
     * @brief 向任务队列添加任务
     * @param task 一个返回值为 void 的可调用对象 (Lambda, std::bind 等)
     */
    void AddTask(std::function<void()> task);

private:
    // 每个工作线程内部运行的函数
    void Worker();

    // 线程容器
    std::vector<std::thread> workers_;
    
    // 任务队列
    std::queue<std::function<void()>> tasks_;
    
    // 同步原语
    std::mutex queue_mutex_;               // 保护任务队列的互斥锁
    std::condition_variable condition_;    // 条件变量，用于线程休眠与唤醒
    
    bool stop_;                            // 线程池停止标志
};

#endif // THREAD_POOL_H