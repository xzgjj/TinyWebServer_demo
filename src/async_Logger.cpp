//

#include "async_logger.h"
#include <cstdio>
#include <cstring>
#include <cerrno>
#include <chrono>
#include <iostream>

AsyncLogger::AsyncLogger(std::string basename, int flush_interval)
    : flush_interval_(flush_interval),
      running_(false),
      thread_started_(false),
      basename_(std::move(basename))
{
    // 在构造函数体内初始化智能指针
    current_buffer_ = NewBuffer();
    next_buffer_ = NewBuffer();
}

AsyncLogger::~AsyncLogger()
{
    if (IsRunning())
    {
        Stop();
    }
}

AsyncLogger::BufferPtr AsyncLogger::NewBuffer()
{
    return std::make_unique<Buffer>();
}

bool AsyncLogger::Append(const char* log_line, size_t len)
{
    if (!IsRunning() || len == 0)
    {
        dropped_logs_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    
    // 尝试写入当前缓冲区
    if (current_buffer_ && current_buffer_->Avail() >= static_cast<int>(len))
    {
        if (current_buffer_->Append(log_line, len))
        {
            return true;
        }
    }

    // 当前缓冲区不足，交换到待写队列
    if (current_buffer_ && current_buffer_->Length() > 0)
    {
        buffers_.push_back(std::move(current_buffer_));
        cond_.notify_one();
    }

    // 使用备用缓冲区
    if (next_buffer_)
    {
        current_buffer_ = std::move(next_buffer_);
    }
    else
    {
        current_buffer_ = NewBuffer();
    }

    // 尝试写入新缓冲区
    if (current_buffer_ && current_buffer_->AppendSafe(log_line, len))
    {
        return true;
    }
    else
    {
        // 单条日志超过缓冲区大小，需要特殊处理
        dropped_logs_.fetch_add(1, std::memory_order_relaxed);
        
        // 创建临时缓冲区处理超大日志
        auto temp_buffer = NewBuffer();
        if (temp_buffer)
        {
            temp_buffer->AppendSafe(log_line, len);
            buffers_.push_back(std::move(temp_buffer));
            cond_.notify_one();
        }
        
        // 创建新的当前缓冲区
        current_buffer_ = NewBuffer();
        return false;
    }
}

bool AsyncLogger::Start()
{
    if (thread_started_.load(std::memory_order_acquire))
    {
        return false;
    }

    running_.store(true, std::memory_order_release);
    thread_ = std::thread(&AsyncLogger::ThreadFunc, this);
    thread_started_.store(true, std::memory_order_release);
    
    return true;
}

void AsyncLogger::Stop()
{
    if (!thread_started_.load(std::memory_order_acquire))
    {
        return;
    }

    running_.store(false, std::memory_order_release);
    cond_.notify_all();
    
    if (thread_.joinable())
    {
        thread_.join();
        thread_started_.store(false, std::memory_order_release);
    }

    // 写入剩余的日志
    WriteBuffersToFile();
}

void AsyncLogger::Flush()
{
    if (!IsRunning())
    {
        return;
    }

    std::unique_lock<std::mutex> lock(mutex_);
    if (current_buffer_ && current_buffer_->Length() > 0)
    {
        buffers_.push_back(std::move(current_buffer_));
        current_buffer_ = NewBuffer();
        cond_.notify_one();
    }
    
    // 等待所有缓冲区写入完成
    flush_cond_.wait(lock, [this]() {
        return buffers_.empty();
    });
}

void AsyncLogger::ThreadFunc()
{
    // 准备后端专用缓冲区
    BufferPtr buffer1 = NewBuffer();
    BufferPtr buffer2 = NewBuffer();
    BufferVector buffers_to_write;
    buffers_to_write.reserve(kMaxBuffersToWrite);

    // 打开日志文件
    FILE* fp = std::fopen(basename_.c_str(), "a");
    if (!fp)
    {
        std::cerr << "Failed to open log file: " << basename_ 
                  << ", error: " << std::strerror(errno) << std::endl;
        running_.store(false, std::memory_order_release);
        return;
    }

    // 设置为行缓冲模式
    std::setvbuf(fp, nullptr, _IOLBF, 0);
    while (running_) {
        {
            std::unique_lock<std::mutex> lock(mutex_);
            if (buffers_.empty()) {
                cond_.wait_for(lock, std::chrono::seconds(flush_interval_));
            }
            // 将前端待写队列交换到后端执行
            buffers_.push_back(std::move(current_buffer_));
            current_buffer_ = std::move(buffer1);
            buffers_to_write.swap(buffers_);
            if (!next_buffer_) {
                next_buffer_ = std::move(buffer2);
            }
        }

        // --- 优化后的核心写入逻辑 ---
        if (!buffers_to_write.empty()) {
            for (auto& buffer : buffers_to_write) {
                if (buffer && buffer->Length() > 0) {
                    // 1. 写入文件
                    size_t n = std::fwrite(buffer->Data(), 1, buffer->Length(), fp);
                    total_written_.fetch_add(n, std::memory_order_relaxed);
                    
                    if (n != static_cast<size_t>(buffer->Length())) {
                        std::cerr << "Failed to write complete log" << std::endl;
                    }

                    // 2. 直接在此处尝试回收 buffer 给 buffer1 或 buffer2
                    buffer->Clear(); // 重置缓冲区指针
                    if (!buffer1) {
                        buffer1 = std::move(buffer);
                    } else if (!buffer2) {
                        buffer2 = std::move(buffer);
                    }
                    // 超过 2 个的 buffer 会随着循环结束和 buffers_to_write.clear() 自动释放
                }
            }
            
            buffers_to_write.clear(); // 清空本轮已处理的队列
            std::fflush(fp);          // 确保 OS 缓存刷入磁盘

            // 3. 关键修复：唤醒可能正在等待 Flush() 的线程
            {
                std::lock_guard<std::mutex> lock(mutex_);
                flush_cond_.notify_all(); 
            }
        }
    }
    

    // 线程结束前，确保写入所有剩余数据
    WriteBuffersToFile();
    
    if (fp)
    {
        std::fclose(fp);
    }
}

void AsyncLogger::WriteBuffersToFile()
{
    BufferVector remaining_buffers;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        
        if (current_buffer_ && current_buffer_->Length() > 0)
        {
            buffers_.push_back(std::move(current_buffer_));
        }
        
        if (!buffers_.empty())
        {
            remaining_buffers.swap(buffers_);
        }
    }

    if (!remaining_buffers.empty())
    {
        FILE* fp = std::fopen(basename_.c_str(), "a");
        if (fp)
        {
            for (const auto& buffer : remaining_buffers)
            {
                if (buffer && buffer->Length() > 0)
                {
                    std::fwrite(buffer->Data(), 1, 
                               static_cast<size_t>(buffer->Length()), fp);
                }
            }
            std::fflush(fp);
            std::fclose(fp);
        }
    }
}