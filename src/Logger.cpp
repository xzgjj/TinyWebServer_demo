//

#include "Logger.h"
#include <cstdarg>
#include <chrono>
#include <ctime>
#include <cstdio>
#include <cstring>

void Logger::Log(LogLevel level, const char* file, int line, const char* fmt, ...)
{
    if (!impl_ || static_cast<int>(level) < static_cast<int>(min_level_.load(std::memory_order_acquire)))
    {
        return;
    }

    // 使用大缓冲区处理可能的长日志
    static constexpr size_t kFormatBufferSize = 8192;
    static constexpr size_t kFinalBufferSize = kFormatBufferSize + 256; // 额外空间用于元数据
    
    char format_buffer[kFormatBufferSize];
    char final_buffer[kFinalBufferSize];
    
    // 1. 格式化时间
    auto now = std::chrono::system_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()) % 1000;
    auto t_c = std::chrono::system_clock::to_time_t(now);
    
    struct tm buf_tm;
#ifdef _WIN32
    localtime_s(&buf_tm, &t_c);
#else
    ::localtime_r(&t_c, &buf_tm);
#endif

    // 2. 格式化用户消息
    va_list args;
    va_start(args, fmt);
    int msg_len = std::vsnprintf(format_buffer, sizeof(format_buffer), fmt, args);
    va_end(args);

    // 确保消息长度不超过缓冲区
    if (msg_len < 0)
    {
        return; // 格式化失败
    }
    
    size_t actual_msg_len = static_cast<size_t>(msg_len);
    if (actual_msg_len >= sizeof(format_buffer))
    {
        actual_msg_len = sizeof(format_buffer) - 1;
    }

    // 3. 构造完整日志行
    int total_len = std::snprintf(
        final_buffer, sizeof(final_buffer),
        "%04d-%02d-%02d %02d:%02d:%02d.%03d [%s] %s:%d - %.*s\n",
        buf_tm.tm_year + 1900, buf_tm.tm_mon + 1, buf_tm.tm_mday,
        buf_tm.tm_hour, buf_tm.tm_min, buf_tm.tm_sec,
        static_cast<int>(ms.count()),
        LevelToString(level),
        file, line,
        static_cast<int>(actual_msg_len), format_buffer);

    // 4. 发送到异步缓冲区
    if (total_len > 0 && static_cast<size_t>(total_len) < sizeof(final_buffer))
    {
        impl_->Append(final_buffer, static_cast<size_t>(total_len));
    }
    else if (total_len > 0)
    {
        // 日志过长，截断处理
        const char truncation_msg[] = " [TRUNCATED]";
        size_t max_len = sizeof(final_buffer) - sizeof(truncation_msg) - 1;
        
        if (max_len > 0)
        {
            // 直接使用原始缓冲区，不需要再格式化
            std::memcpy(final_buffer + max_len, truncation_msg, sizeof(truncation_msg) - 1);
            final_buffer[max_len + sizeof(truncation_msg) - 1] = '\n';
            final_buffer[max_len + sizeof(truncation_msg)] = '\0';
            
            impl_->Append(final_buffer, max_len + sizeof(truncation_msg));
        }
    }

    // 5. FATAL级别特殊处理
    if (level == LogLevel::FATAL)
    {
        Flush();
        std::abort();
    }
}