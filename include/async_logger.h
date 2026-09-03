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
#include <cstdio>
#include "log_buffer.h"

class AsyncLogger 
{
public:
    // 使用枚举值确保编译时常量
    enum { kLargeBuffer = 4000 * 1024 }; // 4MB
    static constexpr int kMaxBuffersToWrite = 16;

    // 【新增】待写队列上限（默认与配置 LoggingOptions.queue_size 一致）
    static constexpr size_t kMaxPendingLogs = 10000;

    // 【新增】日志文件轮转阈值（默认 64MB，超过则保存为 .1 并重开新文件，仅保留 1 份轮转）
    static constexpr size_t kDefaultMaxFileSize = 64 * 1024 * 1024;

    // 【新增】队列满时的处理策略
    enum class FullPolicy {
        kDiscardOldest,   ///< 丢弃最旧的待写缓冲（默认，保证日志最新）
        kDiscardNewest,   ///< 丢弃最新的日志行
        kBlock            ///< 阻塞调用线程等待空间（上限 1s，超时丢弃）
    };

    // 先定义缓冲区类型别名
    using Buffer = logging::FixedBuffer<kLargeBuffer>;
    using BufferPtr = std::unique_ptr<Buffer>;
    using BufferVector = std::vector<BufferPtr>;

    /**
     * @brief 构造函数
     * @param basename 日志文件基础名称
     * @param flush_interval 强制冲刷间隔（秒）
     * @param max_pending 待写队列上限（缓冲个数，0 时使用默认值）
     * @param policy 队列满时的处理策略
     * @param max_file_size 单文件大小阈值（0 表示不轮转）
     */
    explicit AsyncLogger(std::string basename, int flush_interval = 3,
                         size_t max_pending = kMaxPendingLogs,
                         FullPolicy policy = FullPolicy::kDiscardOldest,
                         size_t max_file_size = kDefaultMaxFileSize);
    
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

    /** @brief 当前待写队列中的缓冲数量（调试/测试用，线程安全） */
    size_t GetPendingCount() const;

    /** @brief 当前待写队列中的字节数（调试/测试用，线程安全） */
    size_t GetPendingBytes() const;

    /** @brief 已丢弃的日志数量（队列满策略导致，线程安全） */
    int64_t GetDroppedCount() const {
        return dropped_logs_.load(std::memory_order_relaxed);
    }

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

    /**
     * @brief 确保待写队列有空间（按策略腾挪），必须在持有 mutex_ 时调用
     * @return false 表示本行日志应被丢弃（kDiscardNewest 场景）
     */
    bool EnforceCapacityLocked();

    /**
     * @brief 日志文件轮转：当前文件保存为 basename.1（覆盖旧轮转）并重开新文件
     * @param fp 后端持有的文件指针（轮转后指向新文件）
     * @return 是否轮转成功
     */
    bool RotateFile(std::FILE*& fp);

    /**
     * @brief 丢弃日志告警（每满 100 条输出一次 stderr，丢弃由队列满策略导致）
     */
    void ReportDrop();

    const int flush_interval_;
    std::atomic<bool> running_;
    std::atomic<bool> thread_started_;
    const std::string basename_;

    // 【新增】有界队列参数
    const size_t max_pending_;
    const FullPolicy full_policy_;

    // 【新增】轮转参数与当前文件写入字节
    const size_t max_file_size_;
    std::atomic<int64_t> written_since_rotation_{0};
    
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