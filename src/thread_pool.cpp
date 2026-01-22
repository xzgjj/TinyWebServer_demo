//

#include "thread_pool.h"
#include <iostream>

// 构造函数
ThreadPool::ThreadPool(size_t thread_count) : stop_(false) {
    for (size_t i = 0; i < thread_count; ++i) {
        // 使用 Lambda 表达式创建工作线程
        workers_.emplace_back([this] {
            this->Worker();
        });
    }
}

// 析构函数
ThreadPool::~ThreadPool() {
    {
        // 1. 上锁修改停止标志
        std::unique_lock<std::mutex> lock(queue_mutex_);
        stop_ = true;
    }
    // 2. 唤醒所有正在休眠的线程
    condition_.notify_all();

    // 3. 等待所有线程执行完当前任务并退出
    for (std::thread &worker : workers_) {
        if (worker.joinable()) {
            worker.join();
        }
    }
}

// 添加任务
void ThreadPool::AddTask(std::function<void()> task) {
    {
        std::unique_lock<std::mutex> lock(queue_mutex_);
        if (stop_) return; // 线程池停止后不再接受新任务
        tasks_.emplace(std::move(task));
    }
    // 唤醒一个正在等待任务的线程
    condition_.notify_one();
}

// 工作线程的内部循环
void ThreadPool::Worker() {
    while (true) {
        std::function<void()> task;
        {
            // --- 临界区开始 ---
            std::unique_lock<std::mutex> lock(queue_mutex_);

            // 等待直到：1. 线程池停止  2. 任务队列不为空
            condition_.wait(lock, [this] {
                return stop_ || !tasks_.empty();
            });

            // 如果线程池停止且队列已空，则退出线程
            if (stop_ && tasks_.empty()) {
                return;
            }

            // 从队列头部取出一个任务
            task = std::move(tasks_.front());
            tasks_.pop();
            // --- 临界区结束，自动释放 lock ---
        }

        // 在锁之外执行任务，这样其他线程可以并发地取下一个任务
        if (task) {
            task();
        }
    }
}