//  (轻量化流式辅助)


#ifndef LOG_STREAM_H
#define LOG_STREAM_H

#include <string>
#include <cstring>
#include <algorithm>
#include <cstdio>

namespace logging
{
/**
 * @brief 简单的字符缓冲区，用于流式操作，避免 std::stringstream 的高开销
 */
class LogStream 
{
public:
    static constexpr int kMaxNumericSize = 48;
    static constexpr int kInitialBufferSize = 1024;

    LogStream() 
    {
        buffer_.reserve(kInitialBufferSize);
    }

    LogStream& operator<<(bool v) 
    {
        append(v ? "true" : "false", v ? 4 : 5);
        return *this;
    }

    LogStream& operator<<(short v) 
    {
        char buf[kMaxNumericSize];
        int len = std::snprintf(buf, sizeof(buf), "%hd", v);
        append(buf, static_cast<size_t>(len));
        return *this;
    }

    LogStream& operator<<(unsigned short v) 
    {
        char buf[kMaxNumericSize];
        int len = std::snprintf(buf, sizeof(buf), "%hu", v);
        append(buf, static_cast<size_t>(len));
        return *this;
    }

    LogStream& operator<<(int v) 
    {
        char buf[kMaxNumericSize];
        int len = std::snprintf(buf, sizeof(buf), "%d", v);
        append(buf, static_cast<size_t>(len));
        return *this;
    }

    LogStream& operator<<(unsigned int v) 
    {
        char buf[kMaxNumericSize];
        int len = std::snprintf(buf, sizeof(buf), "%u", v);
        append(buf, static_cast<size_t>(len));
        return *this;
    }

    LogStream& operator<<(long v) 
    {
        char buf[kMaxNumericSize];
        int len = std::snprintf(buf, sizeof(buf), "%ld", v);
        append(buf, static_cast<size_t>(len));
        return *this;
    }

    LogStream& operator<<(unsigned long v) 
    {
        char buf[kMaxNumericSize];
        int len = std::snprintf(buf, sizeof(buf), "%lu", v);
        append(buf, static_cast<size_t>(len));
        return *this;
    }

    LogStream& operator<<(long long v) 
    {
        char buf[kMaxNumericSize];
        int len = std::snprintf(buf, sizeof(buf), "%lld", v);
        append(buf, static_cast<size_t>(len));
        return *this;
    }

    LogStream& operator<<(unsigned long long v) 
    {
        char buf[kMaxNumericSize];
        int len = std::snprintf(buf, sizeof(buf), "%llu", v);
        append(buf, static_cast<size_t>(len));
        return *this;
    }

    LogStream& operator<<(float v) 
    {
        char buf[kMaxNumericSize];
        int len = std::snprintf(buf, sizeof(buf), "%.6g", v);
        append(buf, static_cast<size_t>(len));
        return *this;
    }

    LogStream& operator<<(double v) 
    {
        char buf[kMaxNumericSize];
        int len = std::snprintf(buf, sizeof(buf), "%.12g", v);
        append(buf, static_cast<size_t>(len));
        return *this;
    }

    LogStream& operator<<(long double v) 
    {
        char buf[kMaxNumericSize];
        int len = std::snprintf(buf, sizeof(buf), "%.12Lg", v);
        append(buf, static_cast<size_t>(len));
        return *this;
    }

    LogStream& operator<<(char v) 
    {
        append(&v, 1);
        return *this;
    }

    LogStream& operator<<(const char* str) 
    {
        if (str != nullptr) 
        {
            append(str, std::strlen(str));
        }
        return *this;
    }

    LogStream& operator<<(const std::string& str) 
    {
        append(str.c_str(), str.size());
        return *this;
    }

    LogStream& operator<<(const void* ptr) 
    {
        char buf[kMaxNumericSize];
        int len = std::snprintf(buf, sizeof(buf), "%p", ptr);
        append(buf, static_cast<size_t>(len));
        return *this;
    }

    void append(const char* data, size_t len) 
    {
        buffer_.append(data, len);
    }

    const std::string& str() const { return buffer_; }
    void reset() { buffer_.clear(); }
    
    size_t size() const { return buffer_.size(); }
    bool empty() const { return buffer_.empty(); }

private:
    std::string buffer_; 
};

} // namespace logging
#endif