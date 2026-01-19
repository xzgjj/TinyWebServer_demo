//

#ifndef LOGGER_H
#define LOGGER_H

#include "async_logger.h"
#include <iostream>
#include <string>
#include <atomic>

/**
 * @brief 日志级别定义
 */
enum class LogLevel 
{
    DEBUG,
    INFO,
    WARN,
    ERROR,
    FATAL
};

class Logger 
{
public:
    static Logger& GetInstance() 
    {
        static Logger instance;
        return instance;
    }

    // 设置全局日志文件并启动后端线程
    void Init(const std::string& log_path, LogLevel min_level = LogLevel::INFO) 
    {
        min_level_ = min_level;
        impl_ = std::make_unique<AsyncLogger>(log_path);
        impl_->Start();
    }

    void SetMinLevel(LogLevel level) { min_level_ = level; }
    LogLevel GetMinLevel() const { return min_level_; }

    void Log(LogLevel level, const char* file, int line, const char* fmt, ...);

    void Flush() 
    {
        if (impl_) 
        {
            impl_->Flush();
        }
    }

private:
    Logger() : min_level_(LogLevel::INFO) {}
    std::unique_ptr<AsyncLogger> impl_;
    std::atomic<LogLevel> min_level_;

    const char* LevelToString(LogLevel level) 
    {
        switch (level) 
        {
            case LogLevel::DEBUG: return "DEBUG";
            case LogLevel::INFO:  return "INFO ";
            case LogLevel::WARN:  return "WARN ";
            case LogLevel::ERROR: return "ERROR";
            case LogLevel::FATAL: return "FATAL";
            default: return "UNKNOWN";
        }
    }
};

// 宏定义：自动采集元数据
#define LOG_BASE(level, fmt, ...) \
    do { \
        if (level >= Logger::GetInstance().GetMinLevel()) { \
            Logger::GetInstance().Log(level, __FILE__, __LINE__, fmt, ##__VA_ARGS__); \
        } \
    } while(0)

#define LOG_DEBUG(fmt, ...) LOG_BASE(LogLevel::DEBUG, fmt, ##__VA_ARGS__)
#define LOG_INFO(fmt, ...)  LOG_BASE(LogLevel::INFO,  fmt, ##__VA_ARGS__)
#define LOG_WARN(fmt, ...)  LOG_BASE(LogLevel::WARN,  fmt, ##__VA_ARGS__)
#define LOG_ERROR(fmt, ...) LOG_BASE(LogLevel::ERROR, fmt, ##__VA_ARGS__)
#define LOG_FATAL(fmt, ...) LOG_BASE(LogLevel::FATAL, fmt, ##__VA_ARGS__)

#endif