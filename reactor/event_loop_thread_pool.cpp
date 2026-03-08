//  实现线程池的启动与轮询分发逻辑

#include "reactor/event_loop_thread_pool.h"
#include "reactor/event_loop_thread.h"
#include "Logger.h"

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

EventLoop* EventLoopThreadPool::GetLoopByIndex(size_t index) const {
    if (index >= loops_.size()) {
        return nullptr;
    }
    return loops_[index];
}

void EventLoopThreadPool::AssignListenSockets(const std::vector<int>& listen_fds) {
    // 保存分配的监听socket
    assigned_listen_fds_ = listen_fds;

    // 确保线程池已启动
    if (loops_.empty()) {
        // 如果未启动，先启动线程池
        Start();
    }

    // 检查数量匹配
    size_t num_threads = loops_.size();
    size_t num_sockets = listen_fds.size();

    if (num_sockets < num_threads) {
        // socket数量少于线程数，警告并重复使用
        LOG_WARN("EventLoopThreadPool: only %zu sockets for %zu threads, "
                 "some threads will share sockets", num_sockets, num_threads);
    }

    // 为每个线程分配socket
    for (size_t i = 0; i < num_threads; ++i) {
        size_t socket_idx = i % num_sockets;
        int listen_fd = listen_fds[socket_idx];

        // 这里只是保存映射关系，实际的socket注册由Server类负责
        LOG_DEBUG("EventLoopThreadPool: assigned socket fd=%d to thread %zu",
                  listen_fd, i);
    }
}