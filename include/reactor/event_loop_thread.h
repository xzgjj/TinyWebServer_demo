
// 单个 I/O 线程，实现线程启动即运行 EventLoop

// [file name]: include/reactor/event_loop_thread.h
#ifndef REACTOR_EVENT_LOOP_THREAD_H
#define REACTOR_EVENT_LOOP_THREAD_H

#include "event_loop.h"
#include <thread>
#include <mutex>
#include <condition_variable>

class EventLoopThread {
public:
    EventLoopThread();
    ~EventLoopThread();
    
    EventLoop* StartLoop();
    void Stop();  // 添加 Stop 方法声明
    void Join();  // 添加 Join 方法声明
    
private:
    void ThreadFunc();
    
    EventLoop* loop_;
    std::thread thread_;
    std::mutex mutex_;
    std::condition_variable cond_;
    bool exiting_;
    bool started_;
};

#endif // REACTOR_EVENT_LOOP_THREAD_H
