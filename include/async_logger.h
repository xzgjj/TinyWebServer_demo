//双缓冲异步日志器实现

#ifndef ASYNC_LOGGER_H
#define ASYNC_LOGGER_H

#include <thread>
#include <mutex>
#include <condition_variable>
#include <vector>
#include <string>
#include <memory>
#include <atomic>
#include <cstddef>
#include "log_buffer.h"

class AsyncLogger 
{
public:
    // 使用枚举值确保编译时常量
    enum { kLargeBuffer = 4000 * 1024 }; // 4MB
    static constexpr int kMaxBuffersToWrite = 16;

    // 先定义缓冲区类型别名
    using Buffer = logging::FixedBuffer<kLargeBuffer>;
    using BufferPtr = std::unique_ptr<Buffer>;
    using BufferVector = std::vector<BufferPtr>;

    /**
     * @brief 构造函数
     * @param basename 日志文件基础名称
     * @param flush_interval 强制冲刷间隔（秒）
     */
    explicit AsyncLogger(std::string basename, int flush_interval = 3);
    
    AsyncLogger(const AsyncLogger&) = delete;
    AsyncLogger& operator=(const AsyncLogger&) = delete;
    
    ~AsyncLogger();

    /**
     * @brief 前端调用接口：将日志写入缓冲区
     */
    bool Append(const char* log_line, size_t len);

    /**
     * @brief 启动后端落盘线程
     */
    bool Start();

    /**
     * @brief 停止日志器
     */
    void Stop();

    /**
     * @brief 检查日志器是否运行
     */
    bool IsRunning() const { return running_.load(std::memory_order_acquire); }

    /**
     * @brief 等待所有缓冲区的日志写入完成
     */
    void Flush();

private:
    /**
     * @brief 后端线程执行函数
     */
    void ThreadFunc();

    /**
     * @brief 写入缓冲区到文件
     */
    void WriteBuffersToFile();

    /**
     * @brief 创建新的缓冲区
     */
    BufferPtr NewBuffer();

    const int flush_interval_;
    std::atomic<bool> running_;
    std::atomic<bool> thread_started_;
    const std::string basename_;
    
    std::thread thread_;
    mutable std::mutex mutex_;
    std::condition_variable cond_;
    std::condition_variable flush_cond_;

    BufferPtr current_buffer_;
    BufferPtr next_buffer_;
    BufferVector buffers_;
    std::atomic<int64_t> total_written_{0};
    std::atomic<int64_t> dropped_logs_{0};
};

#endif