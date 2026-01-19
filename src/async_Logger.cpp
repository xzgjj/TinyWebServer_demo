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

    while (running_.load(std::memory_order_acquire))
    {
        {
            std::unique_lock<std::mutex> lock(mutex_);
            
            // 等待前端数据或超时，处理虚假唤醒
            while (buffers_.empty() && running_.load(std::memory_order_acquire))
            {
                cond_.wait_for(lock, std::chrono::seconds(flush_interval_));
            }

            // 检查是否有当前缓冲区需要写入
            if (current_buffer_ && current_buffer_->Length() > 0)
            {
                buffers_.push_back(std::move(current_buffer_));
                current_buffer_ = std::move(buffer1);
                buffer1.reset();
            }

            // 交换待写队列
            if (!buffers_.empty())
            {
                buffers_to_write.swap(buffers_);
                
                // 确保有备用缓冲区
                if (!next_buffer_ && buffer2)
                {
                    next_buffer_ = std::move(buffer2);
                    buffer2.reset();
                }
            }

            // 通知等待Flush的线程
            if (buffers_to_write.empty() && buffers_.empty())
            {
                flush_cond_.notify_all();
            }
        }

        // 临界区外执行磁盘IO
        if (!buffers_to_write.empty())
        {
            for (const auto& buffer : buffers_to_write)
            {
                if (buffer && buffer->Length() > 0)
                {
                    size_t written = std::fwrite(buffer->Data(), 1, 
                                                static_cast<size_t>(buffer->Length()), fp);
                    total_written_.fetch_add(written, std::memory_order_relaxed);
                    
                    if (written != static_cast<size_t>(buffer->Length()))
                    {
                        std::cerr << "Failed to write complete log: " 
                                  << std::strerror(errno) << std::endl;
                    }
                }
            }
            
            // 保留两个缓冲区备用
            if (buffers_to_write.size() > 2)
            {
                buffers_to_write.resize(2);
            }

            // 回收缓冲区
            for (auto& buffer : buffers_to_write)
            {
                if (buffer)
                {
                    if (!buffer1)
                    {
                        buffer1 = std::move(buffer);
                        if (buffer1)
                            buffer1->Clear();
                    }
                    else if (!buffer2)
                    {
                        buffer2 = std::move(buffer);
                        if (buffer2)
                            buffer2->Clear();
                    }
                    else
                    {
                        break; // 已有足够的备用缓冲区
                    }
                }
            }
            
            buffers_to_write.clear();
            
            // 强制刷盘
            std::fflush(fp);
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