//固定大小的缓冲区，用于存放日志条目

/**
 * @file log_buffer.h
 * @brief 固定大小的缓冲区，用于存放日志条目
 */

#ifndef LOG_BUFFER_H
#define LOG_BUFFER_H

#include <algorithm>
#include <cstring>
#include <cstddef>

namespace logging 
{

template <int kSize>
class FixedBuffer 
{
public:
    FixedBuffer() : cur_(data_) 
    {
        std::memset(data_, 0, kSize);
    }

    /**
     * @brief 向缓冲区追加数据
     * @param buf 数据指针
     * @param len 数据长度
     * @return 实际写入的字节数，0表示缓冲区不足
     */
    size_t Append(const char* buf, size_t len) 
    {
        size_t avail = static_cast<size_t>(Avail());
        if (avail >= len) 
        {
            std::memcpy(cur_, buf, len);
            cur_ += len;
            return len;
        }
        return 0;
    }

    /**
     * @brief 安全追加，确保不会溢出
     */
    bool AppendSafe(const char* buf, size_t len) 
    {
        size_t avail = static_cast<size_t>(Avail());
        size_t to_copy = std::min(len, avail);
        if (to_copy > 0) 
        {
            std::memcpy(cur_, buf, to_copy);
            cur_ += to_copy;
        }
        return to_copy == len;
    }

    [[nodiscard]] const char* Data() const noexcept { return data_; }
    [[nodiscard]] int Length() const noexcept { return static_cast<int>(cur_ - data_); }
    [[nodiscard]] char* Current() noexcept { return cur_; }
    [[nodiscard]] int Avail() const noexcept { return static_cast<int>(kSize - (cur_ - data_)); }
    
    void Reset() noexcept { cur_ = data_; }
    void Clear() noexcept 
    { 
        Reset(); 
        std::memset(data_, 0, kSize); 
    }

    [[nodiscard]] bool IsEmpty() const noexcept { return cur_ == data_; }
    [[nodiscard]] bool IsFull() const noexcept { return Avail() <= 0; }

private:
    char data_[kSize];
    char* cur_;
};

} // namespace logging

#endif