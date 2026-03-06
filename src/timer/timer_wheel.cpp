#include "timer/timer_wheel.h"

#include <algorithm>
#include <sstream>

namespace tinywebserver {

TimerWheel::TimerWheel(std::size_t wheel_size, int tick_interval_ms)
    : wheel_(wheel_size),
      current_slot_(0),
      wheel_size_(wheel_size > 0 ? wheel_size : 60),
      tick_interval_ms_(tick_interval_ms > 0 ? tick_interval_ms : 1000) {

    // 预分配哈希表空间以减少重哈希
    timers_.reserve(wheel_size_ * 2);
}

TimerWheel::~TimerWheel() {
    Clear();
}

Error TimerWheel::AddTimeout(int fd, int timeout_seconds, std::function<void()> callback) {
    // 参数验证
    if (fd < 0) {
        return Error(WebError::kInvalidArgument, "Invalid file descriptor");
    }

    if (timeout_seconds <= 0) {
        return Error(WebError::kInvalidArgument, "Timeout must be positive");
    }

    if (!callback) {
        return Error(WebError::kInvalidArgument, "Callback function cannot be null");
    }

    // 计算最大支持的超时时间
    const int max_timeout = static_cast<int>(wheel_size_ * kMaxCycles);
    if (timeout_seconds > max_timeout) {
        std::ostringstream oss;
        oss << "Timeout " << timeout_seconds << "s exceeds maximum " << max_timeout << "s";
        return Error(WebError::kInvalidArgument, oss.str());
    }

    std::lock_guard<std::mutex> lock(mutex_);

    // 如果已存在相同 fd 的定时器，先移除
    if (timers_.find(fd) != timers_.end()) {
        // 静默移除旧定时器，不返回错误
        auto it = timers_[fd];
        std::size_t slot = (current_slot_.load() + it->remaining_ticks) % wheel_size_;
        wheel_[slot].erase(it);
        timers_.erase(fd);
        total_tasks_cancelled_.fetch_add(1, std::memory_order_relaxed);
    }

    // 计算目标槽位和剩余滴答数
    std::size_t slot;
    int remaining_ticks;
    Error calc_error = CalculateSlot(timeout_seconds, slot, remaining_ticks);
    if (calc_error.IsFailure()) {
        return calc_error;
    }

    // 创建定时任务
    TimerTask task(fd, std::move(callback), remaining_ticks);

    // 添加到时间轮
    AddTaskToSlot(slot, std::move(task));

    total_tasks_added_.fetch_add(1, std::memory_order_relaxed);
    return Error::Success();
}

Error TimerWheel::RemoveTimeout(int fd) {
    if (fd < 0) {
        return Error(WebError::kInvalidArgument, "Invalid file descriptor");
    }

    std::lock_guard<std::mutex> lock(mutex_);

    auto it = timers_.find(fd);
    if (it == timers_.end()) {
        // 不存在不是错误，返回成功
        return Error::Success();
    }

    // 从时间轮中移除
    auto task_it = it->second;
    std::size_t slot = (current_slot_.load() + task_it->remaining_ticks) % wheel_size_;
    wheel_[slot].erase(task_it);
    timers_.erase(it);

    total_tasks_cancelled_.fetch_add(1, std::memory_order_relaxed);
    return Error::Success();
}

std::size_t TimerWheel::Tick() {
    std::lock_guard<std::mutex> lock(mutex_);

    std::size_t triggered_count = ProcessSlot(current_slot_.load());

    // 前进到下一个槽位
    current_slot_.store((current_slot_.load() + 1) % wheel_size_);

    return triggered_count;
}

int TimerWheel::GetNextTickTimeout() const {
    std::lock_guard<std::mutex> lock(mutex_);

    // 如果没有任何定时任务，返回 -1 表示不需要定时器
    if (timers_.empty()) {
        return -1;
    }

    // 时间轮以固定间隔前进，返回固定间隔
    return tick_interval_ms_;
}

std::size_t TimerWheel::GetActiveTimerCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return timers_.size();
}

bool TimerWheel::HasTimer(int fd) const {
    if (fd < 0) {
        return false;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    return timers_.find(fd) != timers_.end();
}

void TimerWheel::Clear() {
    std::lock_guard<std::mutex> lock(mutex_);

    // 清空所有槽位
    for (auto& slot : wheel_) {
        slot.clear();
    }

    // 清空映射
    timers_.clear();

    // 重置当前槽位
    current_slot_.store(0);

    // 更新统计
    total_tasks_cancelled_.fetch_add(timers_.size(), std::memory_order_relaxed);
}

std::string TimerWheel::GetStats() const {
    std::lock_guard<std::mutex> lock(mutex_);

    std::ostringstream oss;
    oss << "TimerWheel Stats:\n";
    oss << "  Wheel Size: " << wheel_size_ << " slots\n";
    oss << "  Current Slot: " << current_slot_.load() << "\n";
    oss << "  Active Timers: " << timers_.size() << "\n";
    oss << "  Total Added: " << total_tasks_added_.load(std::memory_order_relaxed) << "\n";
    oss << "  Total Triggered: " << total_tasks_triggered_.load(std::memory_order_relaxed) << "\n";
    oss << "  Total Cancelled: " << total_tasks_cancelled_.load(std::memory_order_relaxed) << "\n";

    // 每个槽位的任务数量分布
    oss << "  Slot Distribution:\n";
    for (std::size_t i = 0; i < wheel_.size(); ++i) {
        if (!wheel_[i].empty()) {
            oss << "    Slot " << i << ": " << wheel_[i].size() << " tasks\n";
        }
    }

    return oss.str();
}

Error TimerWheel::CalculateSlot(int timeout_seconds, std::size_t& slot, int& remaining_ticks) const {
    if (timeout_seconds <= 0) {
        return Error(WebError::kInvalidArgument, "Timeout must be positive");
    }

    // 转换为滴答数（1滴答 = 1秒）
    int ticks = timeout_seconds;

    // 计算目标槽位
    if (ticks <= static_cast<int>(wheel_size_)) {
        // 单圈内超时
        slot = (current_slot_.load() + ticks) % wheel_size_;
        remaining_ticks = 0;
    } else {
        // 多圈超时
        int cycles = ticks / static_cast<int>(wheel_size_);
        if (cycles > static_cast<int>(kMaxCycles)) {
            return Error(WebError::kInvalidArgument, "Timeout exceeds maximum cycles");
        }

        int offset = ticks % static_cast<int>(wheel_size_);
        slot = (current_slot_.load() + offset) % wheel_size_;
        remaining_ticks = cycles;
    }

    return Error::Success();
}

void TimerWheel::AddTaskToSlot(std::size_t slot, TimerTask&& task) {
    // 确保槽位索引有效
    if (slot >= wheel_.size()) {
        slot = slot % wheel_.size();
    }

    // 添加到槽位列表
    auto& slot_list = wheel_[slot];
    slot_list.push_front(std::move(task));

    // 保存迭代器用于快速删除
    timers_[task.fd] = slot_list.begin();
}

std::size_t TimerWheel::ProcessSlot(std::size_t slot) {
    if (slot >= wheel_.size()) {
        return 0;
    }

    std::size_t triggered_count = 0;
    auto& slot_list = wheel_[slot];

    // 遍历当前槽位的所有任务
    auto it = slot_list.begin();
    while (it != slot_list.end()) {
        if (it->remaining_ticks > 0) {
            // 还有剩余圈数，减少圈数并移动到下一轮
            --(it->remaining_ticks);

            // 移动到下一个槽位（下一圈）
            std::size_t next_slot = (slot + 1) % wheel_size_;

            // 保存任务并移除当前迭代器
            TimerTask task = std::move(*it);
            it = slot_list.erase(it);

            // 更新迭代器映射
            timers_.erase(task.fd);

            // 重新添加到下一个槽位
            AddTaskToSlot(next_slot, std::move(task));
        } else {
            // 触发超时回调
            if (it->callback) {
                try {
                    it->callback();
                } catch (const std::exception& e) {
                    // 回调异常不应影响时间轮运行
                    // 在实际项目中，这里应该记录日志
                }
            }

            // 从映射中移除
            timers_.erase(it->fd);

            // 从列表中移除
            it = slot_list.erase(it);

            ++triggered_count;
            total_tasks_triggered_.fetch_add(1, std::memory_order_relaxed);
        }
    }

    return triggered_count;
}

} // namespace tinywebserver