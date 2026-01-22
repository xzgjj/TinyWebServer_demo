//  实现线程池的启动与轮询分发逻辑

#include "reactor/event_loop_thread_pool.h"
#include "reactor/event_loop_thread.h"

EventLoopThreadPool::EventLoopThreadPool(EventLoop* base_loop)
    : base_loop_(base_loop),
      num_threads_(0),
      next_(0) {
    if (base_loop_ == nullptr) {
        throw std::invalid_argument("Base loop cannot be null");
    }
}

void EventLoopThreadPool::SetThreadNum(int num_threads) noexcept {
    num_threads_ = num_threads;
}

// 添加 Start 方法实现
void EventLoopThreadPool::Start() {
    if (num_threads_ <= 0) {
        return; // 如果没有线程，直接返回
    }
    
    for (int i = 0; i < num_threads_; ++i) {
        auto t = std::make_unique<EventLoopThread>();
        loops_.push_back(t->StartLoop());
        threads_.push_back(std::move(t));
    }
}

void EventLoopThreadPool::Stop() {
    // 停止所有 EventLoop
    for (auto loop : loops_) {
        if (loop) {
            loop->Quit();  // 通知 EventLoop 停止
        }
    }
    
    // 等待所有线程结束
    for (auto& thread : threads_) {
        if (thread) {
             thread->Join();
        }
    }
    
    // 清理资源
    threads_.clear();
    loops_.clear();
    next_ = 0;
}


EventLoop* EventLoopThreadPool::GetNextLoop() {
    // 如果没有开启多线程，则返回主 Loop
    EventLoop* loop = base_loop_;
    
    if (!loops_.empty()) {
        loop = loops_[next_];
        next_ = (next_ + 1) % static_cast<int>(loops_.size());
    }
    return loop;
}