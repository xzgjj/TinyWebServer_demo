
// 管理 Sub-Reactors 线程池



// [file name]: include/reactor/event_loop_thread_pool.h
#ifndef REACTOR_EVENT_LOOP_THREAD_POOL_H
#define REACTOR_EVENT_LOOP_THREAD_POOL_H

#include "event_loop.h"
#include "event_loop_thread.h"
#include <vector>
#include <memory>

class EventLoopThreadPool {
public:
    explicit EventLoopThreadPool(EventLoop* base_loop);
    void SetThreadNum(int num_threads) noexcept;
    void Start();
    void Stop();
    void Join();
    EventLoop* GetNextLoop();

    // SO_REUSEPORT 支持
    size_t GetThreadCount() const { return loops_.size(); }
    EventLoop* GetLoopByIndex(size_t index) const;
    void AssignListenSockets(const std::vector<int>& listen_fds);

private:
    EventLoop* base_loop_;
    int num_threads_;
    int next_;
    std::vector<std::unique_ptr<EventLoopThread>> threads_;
    std::vector<EventLoop*> loops_;

    // SO_REUSEPORT 支持：每个线程分配的监听socket
    std::vector<int> assigned_listen_fds_;
};

#endif // REACTOR_EVENT_LOOP_THREAD_POOL_H
