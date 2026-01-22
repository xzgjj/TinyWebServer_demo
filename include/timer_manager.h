//

#ifndef TIMER_MANAGER_H
#define TIMER_MANAGER_H

#include <vector>
#include <chrono>
#include <functional>
#include <unordered_map>

using TimeoutCallback = std::function<void()>;
using Clock = std::chrono::steady_clock;
using TimePoint = Clock::time_point;

struct TimerNode {
    int fd;                 // 对应文件描述符
    TimePoint expire;       // 到期时间点
    TimeoutCallback cb;     // 超时回调函数

    // 必须提供比较运算符，供堆逻辑判断优先级（小根堆）
    bool operator<(const TimerNode& t) const {
        return expire < t.expire;
    }
    bool operator>(const TimerNode& t) const {
        return expire > t.expire;
    }

    // 定义移动赋值操作符，修复 std::swap 匹配问题
    TimerNode& operator=(TimerNode&& other) noexcept {
        if (this != &other) {
            fd = other.fd;
            expire = other.expire;
            cb = std::move(other.cb);
        }
        return *this;
    }

    // 默认构造与移动构造
    TimerNode(int f, TimePoint e, TimeoutCallback c) 
        : fd(f), expire(e), cb(std::move(c)) {}
    TimerNode(TimerNode&&) = default;
};

class TimerManager {
public:
    TimerManager() { heap_.reserve(64); }
    ~TimerManager() = default;

    void AddTimer(int fd, int timeout_ms, const TimeoutCallback& cb);
    void AdjustTimer(int fd, int timeout_ms);
    void RemoveTimer(int fd);
    void HandleExpiredTimers();
    int GetNextTimeout();

private:
    void del_(size_t index);
    void siftup_(size_t i);
    bool siftdown_(size_t index, size_t n);
    void swap_node_(size_t i, size_t j);

    std::vector<TimerNode> heap_;
    // 映射 fd 到堆索引，实现 O(1) 查找
    std::unordered_map<int, size_t> ref_; 
};

#endif