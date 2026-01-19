//

#include "Logger.h"
#include <cstdarg>
#include <cstdio>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <chrono>

// 辅助函数：格式化时间
static std::string CurrentTime() {
    auto now = std::chrono::system_clock::now();
    auto now_time = std::chrono::system_clock::to_time_t(now);
    auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()) % 1000;
    
    std::ostringstream oss;
    oss << std::put_time(std::localtime(&now_time), "%Y-%m-%d %H:%M:%S")
        << '.' << std::setfill('0') << std::setw(3) << now_ms.count();
    return oss.str();
}

void Logger::Log(LogLevel level, const char* file, int line, const char* fmt, ...) {
    if (!impl_ || level < min_level_) {
        return;
    }
    
    // 提取文件名（不含路径）
    const char* filename = file;
    if (const char* slash = strrchr(file, '/')) {
        filename = slash + 1;
    } else if (const char* backslash = strrchr(file, '\\')) {
        filename = backslash + 1;
    }
    
    // 格式化日志前缀
    char prefix[512];
    snprintf(prefix, sizeof(prefix), "[%s] [%s] [%s:%d] ",
             CurrentTime().c_str(),
             LevelToString(level),
             filename, line);
    
    // 格式化用户消息
    char message[4096];
    va_list args;
    va_start(args, fmt);
    int len = vsnprintf(message, sizeof(message), fmt, args);
    va_end(args);
    
    if (len >= 0) {
        // 确保以换行符结尾
        if (len == 0 || message[len-1] != '\n') {
            message[len] = '\n';
            message[len+1] = '\0';
        }
        
        // 构建完整的日志行
        std::string log_line = std::string(prefix) + message;
        
        // 输出到异步日志器
        impl_->Append(log_line.c_str(), log_line.length());
        
        // FATAL 级别立即刷新
        if (level == LogLevel::LOG_LEVEL_FATAL) {
            Flush();
        }
    }
}