//

#include "reactor/event_loop_thread.h"

EventLoopThread::EventLoopThread()
    : loop_(nullptr),
      thread_(),
      mutex_(),
      cond_() {
}

EventLoopThread::~EventLoopThread() {
    if (loop_ != nullptr) {
        loop_->Quit();
        if (thread_.joinable()) {
            thread_.join();
        }
    }
}

EventLoop* EventLoopThread::StartLoop() {
    // 启动线程
    thread_ = std::thread([this]() { this->ThreadFunc(); });
    
    EventLoop* loop = nullptr;
    {
        std::unique_lock<std::mutex> lock(mutex_);
        // 等待线程内部初始化好 EventLoop 对象
        cond_.wait(lock, [this]() { return this->loop_ != nullptr; });
        loop = loop_;
    }
    
    return loop;
}

void EventLoopThread::ThreadFunc() {
    // 在子线程栈上创建 EventLoop，确保生命周期与线程同步
    EventLoop loop;
    
    {
        std::lock_guard<std::mutex> lock(mutex_);
        loop_ = &loop;
        cond_.notify_one();
    }
    
    // 进入 epoll_wait 循环
    loop.Loop();
    
    // 循环结束，清理指针
    std::lock_guard<std::mutex> lock(mutex_);
    loop_ = nullptr;
}

// 添加 Stop 和 Join 方法
void EventLoopThread::Stop() {
    exiting_ = true;
    if (loop_) {
        loop_->Quit();
    }
}

void EventLoopThread::Join() {
    if (thread_.joinable()) {
        thread_.join();
    }
}