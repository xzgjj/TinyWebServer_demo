#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <list>
#include <unordered_map>
#include <vector>
#include <mutex>
#include <chrono>
#include <atomic>

#include "error/error.h"

/**
 * @file timer_wheel.h
 * @brief 时间轮定时器管理器
 *
 * 实现一个固定精度（1秒）的时间轮，用于管理连接超时等定时任务。
 * 时间轮算法提供 O(1) 的添加/删除/触发复杂度。
 */

namespace tinywebserver {

/**
 * @brief 时间轮定时器任务
 */
struct TimerTask {
    int fd;                             ///< 关联的文件描述符
    std::function<void()> callback;     ///< 超时回调函数
    int remaining_ticks;                ///< 剩余滴答数（当超时时间超过轮子大小时使用）
    std::chrono::steady_clock::time_point created_at;  ///< 创建时间戳

    TimerTask(int f, std::function<void()> cb, int ticks)
        : fd(f),
          callback(std::move(cb)),
          remaining_ticks(ticks),
          created_at(std::chrono::steady_clock::now()) {
    }
};

/**
 * @brief 时间轮定时器管理器
 *
 * 实现一个固定大小的循环时间轮，支持秒级精度定时任务。
 * 默认时间轮大小为 60 秒，支持通过构造函数调整。
 *
 * 线程安全：所有公共方法都是线程安全的。
 */
class TimerWheel {
public:
    /**
     * @brief 构造函数
     * @param wheel_size 时间轮大小（秒），默认 60 秒
     * @param tick_interval_ms 滴答间隔（毫秒），默认 1000 毫秒（1秒）
     */
    explicit TimerWheel(std::size_t wheel_size = 60, int tick_interval_ms = 1000);

    TimerWheel(const TimerWheel&) = delete;
    TimerWheel& operator=(const TimerWheel&) = delete;

    /**
     * @brief 析构函数
     */
    ~TimerWheel();

    /**
     * @brief 添加定时任务
     * @param fd 关联的文件描述符
     * @param timeout_seconds 超时时间（秒）
     * @param callback 超时回调函数
     * @return 错误对象，成功时返回 Error::Success()
     *
     * 如果 fd 已存在，会先移除旧的定时任务。
     * 超时时间必须大于 0，小于等于最大支持时间（wheel_size * max_cycles）。
     */
    Error AddTimeout(int fd, int timeout_seconds, std::function<void()> callback);

    /**
     * @brief 移除定时任务
     * @param fd 关联的文件描述符
     * @return 错误对象，成功时返回 Error::Success()
     */
    Error RemoveTimeout(int fd);

    /**
     * @brief 执行一个滴答（前进一个时间槽）
     *
     * 通常由外部定时器每秒调用一次，触发当前槽位的超时任务。
     * 返回触发的任务数量。
     */
    std::size_t Tick();

    /**
     * @brief 获取下一个滴答的等待时间
     * @return 距离下一个滴答的毫秒数，如果时间轮为空则返回 -1
     */
    int GetNextTickTimeout() const;

    /**
     * @brief 获取当前槽位索引
     * @return 当前槽位索引
     */
    std::size_t GetCurrentSlot() const { return current_slot_.load(); }

    /**
     * @brief 获取时间轮大小
     * @return 时间轮大小（槽位数）
     */
    std::size_t GetWheelSize() const { return wheel_.size(); }

    /**
     * @brief 获取活跃定时任务数量
     * @return 活跃定时任务数量
     */
    std::size_t GetActiveTimerCount() const;

    /**
     * @brief 检查是否存在指定 fd 的定时任务
     * @param fd 文件描述符
     * @return true 如果存在定时任务
     */
    bool HasTimer(int fd) const;

    /**
     * @brief 清空所有定时任务
     */
    void Clear();

    /**
     * @brief 获取时间轮统计信息
     * @return 包含统计信息的字符串
     */
    std::string GetStats() const;

private:
    /**
     * @brief 计算超时时间对应的槽位和剩余滴答数
     * @param timeout_seconds 超时时间（秒）
     * @param slot 输出参数：目标槽位
     * @param remaining_ticks 输出参数：剩余滴答数
     * @return 错误对象
     */
    Error CalculateSlot(int timeout_seconds, std::size_t& slot, int& remaining_ticks) const;

    /**
     * @brief 内部添加任务到指定槽位
     */
    void AddTaskToSlot(std::size_t slot, TimerTask&& task);

    /**
     * @brief 执行指定槽位的所有超时任务
     * @param slot 槽位索引
     * @return 触发的任务数量
     */
    std::size_t ProcessSlot(std::size_t slot);

    /// 时间轮槽位数组
    std::vector<std::list<TimerTask>> wheel_;

    /// fd 到任务迭代器的映射（用于快速删除）
    std::unordered_map<int, std::list<TimerTask>::iterator> timers_;

    /// 当前槽位索引（原子操作保证线程安全）
    std::atomic<std::size_t> current_slot_;

    /// 互斥锁保护数据结构
    mutable std::mutex mutex_;

    /// 时间轮大小
    const std::size_t wheel_size_;

    /// 滴答间隔（毫秒）
    const int tick_interval_ms_;

    /// 最大支持循环次数（用于处理超过时间轮大小的超时）
    static constexpr std::size_t kMaxCycles = 10;

    /// 统计信息
    std::atomic<std::size_t> total_tasks_added_{0};
    std::atomic<std::size_t> total_tasks_triggered_{0};
    std::atomic<std::size_t> total_tasks_cancelled_{0};
};

} // namespace tinywebserver