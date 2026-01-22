
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
    
private:
    EventLoop* base_loop_;
    int num_threads_;
    int next_;
    std::vector<std::unique_ptr<EventLoopThread>> threads_;
    std::vector<EventLoop*> loops_;
};

#endif // REACTOR_EVENT_LOOP_THREAD_POOL_H
