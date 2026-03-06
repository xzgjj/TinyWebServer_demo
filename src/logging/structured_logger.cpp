#include "logging/structured_logger.h"
#include "async_Logger.h"
#include "config/server_config.h"
#include <iomanip>
#include <sstream>
#include <iostream>
#include <mutex>

namespace tinywebserver {

// ============================================================================
// StructuredLogEntry 实现
// ============================================================================

std::string StructuredLogEntry::ToText() const {
    std::ostringstream oss;

    // 时间戳
    auto time_t = std::chrono::system_clock::to_time_t(timestamp);
    std::tm tm_buf;
    localtime_r(&time_t, &tm_buf);
    oss << std::put_time(&tm_buf, "%Y-%m-%d %H:%M:%S");

    // 日志级别
    const char* level_str = "";
    switch (level) {
        case StructuredLogLevel::DEBUG: level_str = "DEBUG"; break;
        case StructuredLogLevel::INFO:  level_str = "INFO";  break;
        case StructuredLogLevel::WARN:  level_str = "WARN";  break;
        case StructuredLogLevel::ERROR: level_str = "ERROR"; break;
        case StructuredLogLevel::FATAL: level_str = "FATAL"; break;
    }

    oss << " [" << level_str << "] ";

    // 位置信息
    oss << "[" << file << ":" << line << ":" << function << "] ";

    // 消息
    oss << message;

    // 上下文信息（如果存在）
    if (request_id.has_value()) {
        oss << " [req_id=" << request_id.value() << "]";
    }
    if (connection_id.has_value()) {
        oss << " [conn_id=" << connection_id.value() << "]";
    }
    if (client_ip.has_value()) {
        oss << " [client_ip=" << client_ip.value() << "]";
    }
    if (response_status.has_value()) {
        oss << " [status=" << response_status.value() << "]";
    }

    return oss.str();
}

std::string StructuredLogEntry::ToJson() const {
    std::ostringstream oss;
    oss << "{";

    // 时间戳（ISO 8601格式）
    auto time_t = std::chrono::system_clock::to_time_t(timestamp);
    std::tm tm_buf;
    gmtime_r(&time_t, &tm_buf);
    oss << "\"timestamp\": \"" << std::put_time(&tm_buf, "%Y-%m-%dT%H:%M:%SZ") << "\", ";

    // 日志级别
    const char* level_str = "";
    switch (level) {
        case StructuredLogLevel::DEBUG: level_str = "DEBUG"; break;
        case StructuredLogLevel::INFO:  level_str = "INFO";  break;
        case StructuredLogLevel::WARN:  level_str = "WARN";  break;
        case StructuredLogLevel::ERROR: level_str = "ERROR"; break;
        case StructuredLogLevel::FATAL: level_str = "FATAL"; break;
    }
    oss << "\"level\": \"" << level_str << "\", ";

    // 位置信息
    oss << "\"file\": \"" << file << "\", ";
    oss << "\"line\": " << line << ", ";
    oss << "\"function\": \"" << function << "\", ";

    // 消息（需要转义 JSON 特殊字符）
    std::string escaped_message;
    for (char c : message) {
        switch (c) {
            case '"': escaped_message += "\\\""; break;
            case '\\': escaped_message += "\\\\"; break;
            case '\b': escaped_message += "\\b"; break;
            case '\f': escaped_message += "\\f"; break;
            case '\n': escaped_message += "\\n"; break;
            case '\r': escaped_message += "\\r"; break;
            case '\t': escaped_message += "\\t"; break;
            default: escaped_message += c; break;
        }
    }
    oss << "\"message\": \"" << escaped_message << "\"";

    // 上下文信息
    if (request_id.has_value()) {
        oss << ", \"request_id\": \"" << request_id.value() << "\"";
    }
    if (connection_id.has_value()) {
        oss << ", \"connection_id\": " << connection_id.value();
    }
    if (client_ip.has_value()) {
        oss << ", \"client_ip\": \"" << client_ip.value() << "\"";
    }
    if (user_agent.has_value()) {
        std::string escaped_ua;
        for (char c : user_agent.value()) {
            if (c == '"') escaped_ua += "\\\"";
            else if (c == '\\') escaped_ua += "\\\\";
            else escaped_ua += c;
        }
        oss << ", \"user_agent\": \"" << escaped_ua << "\"";
    }
    if (response_status.has_value()) {
        oss << ", \"response_status\": " << response_status.value();
    }

    oss << "}";
    return oss.str();
}

std::string StructuredLogEntry::ToColoredText() const {
    std::string text = ToText();

    // 简单的 ANSI 颜色代码
    const char* color_code = "";
    switch (level) {
        case StructuredLogLevel::DEBUG: color_code = "\033[36m"; break; // 青色
        case StructuredLogLevel::INFO:  color_code = "\033[32m"; break; // 绿色
        case StructuredLogLevel::WARN:  color_code = "\033[33m"; break; // 黄色
        case StructuredLogLevel::ERROR: color_code = "\033[31m"; break; // 红色
        case StructuredLogLevel::FATAL: color_code = "\033[35m"; break; // 洋红色
        default: color_code = "\033[0m"; break; // 默认无色
    }

    std::string colored = color_code;
    colored += text;
    colored += "\033[0m"; // 重置颜色
    return colored;
}

// ============================================================================
// StructuredLogManager 实现
// ============================================================================

StructuredLogManager& StructuredLogManager::GetInstance() {
    static StructuredLogManager instance;
    return instance;
}

void StructuredLogManager::Init(std::unique_ptr<StructuredLogger> logger) {
    std::lock_guard<std::mutex> lock(mutex_);
    logger_ = std::move(logger);
}

void StructuredLogManager::Log(const StructuredLogEntry& entry) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (logger_) {
        logger_->Log(entry);
    }
}

// ============================================================================
// 简单实现：基于现有 AsyncLogger 的结构化日志器
// ============================================================================

class AsyncStructuredLogger : public StructuredLogger {
public:
    AsyncStructuredLogger(const std::string& log_path, const std::string& format = "text", int flush_interval = 3)
        : format_(format),
          async_logger_(std::make_unique<AsyncLogger>(log_path, flush_interval)) {
        // 启动异步日志器
        async_logger_->Start();
    }

    void Log(const StructuredLogEntry& entry) override {
        std::string formatted;
        if (format_ == "json") {
            formatted = entry.ToJson();
        } else if (format_ == "colored") {
            formatted = entry.ToColoredText();
        } else {
            formatted = entry.ToText();
        }

        // 添加换行符（如果还没有）
        if (!formatted.empty() && formatted.back() != '\n') {
            formatted += '\n';
        }

        // 通过 AsyncLogger 写入
        async_logger_->Append(formatted.c_str(), formatted.size());
    }

    void Flush() override {
        async_logger_->Flush();
    }

    void SetOutputFormat(const std::string& format) override {
        format_ = format;
    }

    // 停止日志器（用于清理）
    void Stop() {
        async_logger_->Stop();
    }

private:
    std::string format_;
    std::unique_ptr<AsyncLogger> async_logger_;
};

// ============================================================================
// 辅助函数
// ============================================================================

/**
 * @brief 从 ServerConfig 初始化结构化日志系统
 */
void InitStructuredLoggerFromConfig(const std::shared_ptr<ServerConfig>& config) {
    if (!config) {
        return;
    }

    auto logging_opts = config->GetLoggingOptions();

    // 只有启用异步日志时才初始化结构化日志
    if (logging_opts.async) {
        std::string format = "text"; // 默认格式
        // TODO: 可以从配置中添加格式选项

        auto logger = std::make_unique<AsyncStructuredLogger>(
            logging_opts.file,
            format,
            logging_opts.flush_interval
        );

        StructuredLogManager::GetInstance().Init(std::move(logger));
    }
}

} // namespace tinywebserver