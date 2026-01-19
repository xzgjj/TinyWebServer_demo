
//负责事件分发、跨线程任务执行

#ifndef TINYWEBSERVER_REACTOR_EVENT_LOOP_H_
#define TINYWEBSERVER_REACTOR_EVENT_LOOP_H_

#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <unistd.h>

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <vector>
#include <thread>
#include <unordered_map>

/**
 * @brief 核心事件循环类 (One Loop Per Thread)
 */
class EventLoop {
public:
    using Functor = std::function<void()>;

    EventLoop();
    ~EventLoop();

    EventLoop(const EventLoop&) = delete;
    EventLoop& operator=(const EventLoop&) = delete;

    std::string GetThreadIdString() const;

    void Loop();
    void Quit();
    void Stop() { quit_ = true; Wakeup(); }

    // 线程安全：支持跨线程调用
    void RunInLoop(Functor cb);
    void QueueInLoop(Functor cb);

    // 事件管理
    void UpdateEvent(int fd, uint32_t events);
    void RemoveEvent(int fd);
    
    // 设置回调
    void SetAcceptCallback(Functor cb) { accept_callback_ = std::move(cb); }
    void SetReadCallback(std::function<void(int)> cb) { read_callback_ = std::move(cb); }
    void SetWriteCallback(std::function<void(int)> cb) { write_callback_ = std::move(cb); }

    bool IsInLoopThread() const noexcept {
        return thread_id_ == std::this_thread::get_id();
    }

private:
    void Wakeup();
    void HandleReadForWakeup();
    void DoPendingFunctors();
    static int CreateEventFd();
    
    void ProcessEvents(int timeout_ms = -1);
    void HandleEvent(int fd, uint32_t events);

    std::atomic<bool> looping_;
    std::atomic<bool> quit_;
    std::atomic<bool> calling_pending_functors_;
    
    const std::thread::id thread_id_;
    
    int epoll_fd_;
    int wakeup_fd_;
    
    static constexpr int kMaxEvents = 1024;
    struct epoll_event events_[kMaxEvents];

    mutable std::mutex mutex_;
    std::vector<Functor> pending_functors_;
    
    // 事件回调
    Functor accept_callback_;
    std::function<void(int)> read_callback_;
    std::function<void(int)> write_callback_;
    
    // 记录已注册的文件描述符
    std::unordered_map<int, uint32_t> registered_fds_;
};

#endif