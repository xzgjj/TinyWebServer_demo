//

#ifndef LOGGER_H
#define LOGGER_H

#include "async_logger.h"
#include <iostream>
#include <string>
#include <atomic>

/**
 * @brief 日志级别定义
 * 注意：避免使用 DEBUG, INFO, ERROR 等可能与系统宏冲突的名称
 */
enum class LogLevel 
{
    LOG_LEVEL_DEBUG,   // 改为 LOG_LEVEL_DEBUG，避免与 DEBUG 宏冲突
    LOG_LEVEL_INFO,    // 改为 LOG_LEVEL_INFO，避免与 INFO 宏冲突
    LOG_LEVEL_WARN,    // 改为 LOG_LEVEL_WARN
    LOG_LEVEL_ERROR,   // 改为 LOG_LEVEL_ERROR，避免与 ERROR 宏冲突
    LOG_LEVEL_FATAL    // 改为 LOG_LEVEL_FATAL
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
    void Init(const std::string& log_path, LogLevel min_level = LogLevel::LOG_LEVEL_INFO) 
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

     std::string GetThreadIdString() const;

private:
    Logger() : min_level_(LogLevel::LOG_LEVEL_INFO) {}
    std::unique_ptr<AsyncLogger> impl_;
    std::atomic<LogLevel> min_level_;

    const char* LevelToString(LogLevel level) 
    {
        switch (level) 
        {
            case LogLevel::LOG_LEVEL_DEBUG: return "DEBUG";
            case LogLevel::LOG_LEVEL_INFO:  return "INFO ";
            case LogLevel::LOG_LEVEL_WARN:  return "WARN ";
            case LogLevel::LOG_LEVEL_ERROR: return "ERROR";
            case LogLevel::LOG_LEVEL_FATAL: return "FATAL";
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

// 更新宏定义，使用新的枚举值
#define LOG_DEBUG(fmt, ...) LOG_BASE(LogLevel::LOG_LEVEL_DEBUG, fmt, ##__VA_ARGS__)
#define LOG_INFO(fmt, ...)  LOG_BASE(LogLevel::LOG_LEVEL_INFO,  fmt, ##__VA_ARGS__)
#define LOG_WARN(fmt, ...)  LOG_BASE(LogLevel::LOG_LEVEL_WARN,  fmt, ##__VA_ARGS__)
#define LOG_ERROR(fmt, ...) LOG_BASE(LogLevel::LOG_LEVEL_ERROR, fmt, ##__VA_ARGS__)
#define LOG_FATAL(fmt, ...) LOG_BASE(LogLevel::LOG_LEVEL_FATAL, fmt, ##__VA_ARGS__)

#endif