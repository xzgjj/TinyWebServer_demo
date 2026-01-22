
//实现跨线程唤醒与任务分发逻辑。


#include "reactor/event_loop.h"
#include "Logger.h"

#include <sys/eventfd.h>
#include <sys/epoll.h>
#include <unistd.h>
#include <sstream>
#include <thread>
#include <cerrno>
#include <cassert>

EventLoop::EventLoop() 
    : looping_(false),
      quit_(false),
      calling_pending_functors_(false),
      thread_id_(std::this_thread::get_id()),
      epoll_fd_(epoll_create1(EPOLL_CLOEXEC)),
      wakeup_fd_(CreateEventFd()) {
    
    if (epoll_fd_ < 0) {
        LOG_FATAL("EventLoop: epoll_create1 failed");
    }
    
    // 注册 wakeup_fd 的读事件，防止 Loop 阻塞
    UpdateEvent(wakeup_fd_, EPOLLIN);
}

EventLoop::~EventLoop() {
    close(epoll_fd_);
    close(wakeup_fd_);
}

void EventLoop::Loop() {
    assert(!looping_);
    thread_id_ = std::this_thread::get_id(); // 在此处正式绑定执行线程
    looping_ = true;
    
    looping_ = true;
    quit_ = false;
    
    while (!quit_) {
        ProcessEvents(10); // 将 1000ms 改为 10ms，提高退出响应速度
        DoPendingFunctors();
    }
    
    looping_ = false;
}

void EventLoop::Quit() {
    quit_ = true;
    if (!IsInLoopThread()) {
        Wakeup();
    }
}

void EventLoop::RunInLoop(Functor cb) {
    if (IsInLoopThread()) {
        cb();
    } else {
        QueueInLoop(std::move(cb));
    }
}

std::string EventLoop::GetThreadIdString() const {
    std::ostringstream oss;
    oss << thread_id_;
    return oss.str();
}

void EventLoop::QueueInLoop(Functor cb) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        pending_functors_.emplace_back(std::move(cb));
    }
    // 如果不在当前线程，或者当前线程正在执行 pending functors，都需要唤醒
    if (!IsInLoopThread() || calling_pending_functors_) {
        Wakeup();
    }
}

void EventLoop::UpdateEvent(int fd, uint32_t events) {
    // 1. 检查当前执行线程是否为该 EventLoop 绑定的 IO 线程
    if (IsInLoopThread()) {
        // 如果在 IO 线程，直接执行操作
        struct epoll_event ev;
        ev.events = events;
        ev.data.fd = fd;
        
        auto it = registered_fds_.find(fd);
        if (it == registered_fds_.end()) {
            // 新注册
            if (epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, fd, &ev) < 0) {
                LOG_ERROR("EventLoop::UpdateEvent epoll_ctl ADD failed for fd=%d", fd);
                return;
            }
            registered_fds_[fd] = events;
        } else {
            // 修改
            if (epoll_ctl(epoll_fd_, EPOLL_CTL_MOD, fd, &ev) < 0) {
                LOG_ERROR("EventLoop::UpdateEvent epoll_ctl MOD failed for fd=%d", fd);
                return;
            }
            registered_fds_[fd] = events;
        }
    } else {
        // 2. 如果不在 IO 线程，通过 RunInLoop 将操作转移（Dispatch）到 IO 线程执行
        // 确保对 epoll_fd_ 和 registered_fds_ 的访问是单线程串行的
        RunInLoop([this, fd, events]() {
            this->UpdateEvent(fd, events);
        });
    }
}

void EventLoop::RemoveEvent(int fd) {
    auto it = registered_fds_.find(fd);
    if (it != registered_fds_.end()) {
        if (epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, fd, nullptr) < 0) {
            LOG_ERROR("EventLoop::RemoveEvent epoll_ctl DEL failed for fd=%d", fd);
        }
        registered_fds_.erase(it);
    }
}

void EventLoop::Wakeup() {
    uint64_t one = 1;
    ssize_t n = write(wakeup_fd_, &one, sizeof(one));
    if (n != sizeof(one)) {
        LOG_ERROR("EventLoop::Wakeup writes %ld bytes instead of 8", n);
    }
}

void EventLoop::HandleReadForWakeup() {
    uint64_t one = 1;
    ssize_t n = read(wakeup_fd_, &one, sizeof(one));
    if (n != sizeof(one)) {
        LOG_ERROR("EventLoop::HandleReadForWakeup reads %lu bytes instead of 8", n);
    }
}

void EventLoop::DoPendingFunctors() {
    std::vector<Functor> functors;
    calling_pending_functors_ = true;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        functors.swap(pending_functors_);
    }

    for (const auto& func : functors) {
        func();
    }
    calling_pending_functors_ = false;
}

int EventLoop::CreateEventFd() {
    int fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (fd < 0) {
        LOG_FATAL("EventLoop::CreateEventFd failed");
    }
    return fd;
}

void EventLoop::ProcessEvents(int timeout_ms) {
    int num_events = epoll_wait(epoll_fd_, events_, kMaxEvents, timeout_ms);
    if (num_events < 0) {
        if (errno != EINTR) {
            LOG_ERROR("EventLoop::ProcessEvents epoll_wait error");
        }
        return;
    }
    
    for (int i = 0; i < num_events; ++i) {
        int fd = events_[i].data.fd;
        uint32_t revents = events_[i].events;
        
        if (fd == wakeup_fd_) {
            if (revents & EPOLLIN) {
                HandleReadForWakeup();
            }
        } else {
            HandleEvent(fd, revents);
        }
    }
}

void EventLoop::HandleEvent(int fd, uint32_t events) {
    // 根据事件类型调用相应的回调
    if ((events & EPOLLIN) && read_callbacks_.count(fd)) {
        read_callbacks_[fd](fd);
    }
    if ((events & EPOLLOUT) && write_callbacks_.count(fd)) {
        write_callbacks_[fd](fd);
    }
    // 注意：这里简化处理，实际应该处理EPOLLERR和EPOLLHUP
    if (events & (EPOLLERR | EPOLLHUP)) {
        LOG_WARN("EventLoop::HandleEvent error/hup events on fd=%d", fd);
        RemoveEvent(fd);
    }
}