#pragma once

#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <optional>

namespace tinywebserver {

// 日志级别（复用现有的 LogLevel，但这里重新定义以避免依赖）
enum class StructuredLogLevel {
    DEBUG,
    INFO,
    WARN,
    ERROR,
    FATAL
};

/**
 * @brief 结构化日志条目
 *
 * 包含完整的日志上下文信息，支持转换为文本或 JSON 格式。
 */
struct StructuredLogEntry {
    std::chrono::system_clock::time_point timestamp;
    StructuredLogLevel level;
    std::string message;
    std::string file;
    int line;
    std::string function;

    // 请求上下文（如果可用）
    std::optional<std::string> request_id;
    std::optional<int> connection_id;
    std::optional<std::string> client_ip;
    std::optional<std::string> user_agent;
    std::optional<int> response_status;

    /**
     * @brief 转换为文本格式（兼容现有日志格式）
     */
    std::string ToText() const;

    /**
     * @brief 转换为 JSON 格式
     */
    std::string ToJson() const;

    /**
     * @brief 转换为日志行字符串（带颜色标记）
     */
    std::string ToColoredText() const;
};

/**
 * @brief 结构化日志器接口
 */
class StructuredLogger {
public:
    virtual ~StructuredLogger() = default;

    /**
     * @brief 记录结构化日志条目
     */
    virtual void Log(const StructuredLogEntry& entry) = 0;

    /**
     * @brief 刷新日志缓冲区
     */
    virtual void Flush() = 0;

    /**
     * @brief 设置输出格式
     */
    virtual void SetOutputFormat(const std::string& format) = 0; // "text", "json", "colored"
};

/**
 * @brief 全局结构化日志管理器
 */
class StructuredLogManager {
public:
    static StructuredLogManager& GetInstance();

    /**
     * @brief 初始化结构化日志器
     */
    void Init(std::unique_ptr<StructuredLogger> logger);

    /**
     * @brief 记录日志条目
     */
    void Log(const StructuredLogEntry& entry);

    /**
     * @brief 获取当前日志器
     */
    StructuredLogger* GetLogger() const { return logger_.get(); }

    /**
     * @brief 检查是否已初始化
     */
    bool IsInitialized() const { return logger_ != nullptr; }

private:
    StructuredLogManager() = default;
    std::unique_ptr<StructuredLogger> logger_;
    std::mutex mutex_;
};

// 前向声明
class ServerConfig;

/**
 * @brief 从 ServerConfig 初始化结构化日志系统
 */
void InitStructuredLoggerFromConfig(const std::shared_ptr<ServerConfig>& config);

// ============================================================================
// 结构化日志宏
// ============================================================================

/**
 * @brief 结构化日志宏（基础版本）
 */
#define LOG_STRUCTURED(lvl, ...) \
    do { \
        if (auto* manager = &::tinywebserver::StructuredLogManager::GetInstance(); \
            manager->IsInitialized()) { \
            ::tinywebserver::StructuredLogEntry entry; \
            entry.timestamp = std::chrono::system_clock::now(); \
            entry.level = static_cast<decltype(entry.level)>(lvl); \
            entry.file = __FILE__; \
            entry.line = __LINE__; \
            entry.function = __func__; \
            /* 消息格式化 */ \
            char _formatted_message[4096]; \
            snprintf(_formatted_message, sizeof(_formatted_message), __VA_ARGS__); \
            entry.message = _formatted_message; \
            manager->Log(entry); \
        } \
    } while(0)

/**
 * @brief 结构化日志宏（带请求上下文）
 */
#define LOG_STRUCTURED_CTX(lvl, request_context, ...) \
    do { \
        if (auto* manager = &::tinywebserver::StructuredLogManager::GetInstance(); \
            manager->IsInitialized()) { \
            ::tinywebserver::StructuredLogEntry entry; \
            entry.timestamp = std::chrono::system_clock::now(); \
            entry.level = static_cast<decltype(entry.level)>(lvl); \
            entry.file = __FILE__; \
            entry.line = __LINE__; \
            entry.function = __func__; \
            /* 消息格式化 */ \
            char _formatted_message[4096]; \
            snprintf(_formatted_message, sizeof(_formatted_message), __VA_ARGS__); \
            entry.message = _formatted_message; \
            /* 请求上下文 */ \
            entry.request_id = (request_context).request_id; \
            entry.connection_id = (request_context).connection_id; \
            entry.client_ip = (request_context).client_ip; \
            entry.user_agent = (request_context).user_agent; \
            entry.response_status = (request_context).response_status; \
            manager->Log(entry); \
        } \
    } while(0)

// 便捷宏
#define LOG_S_DEBUG(...) LOG_STRUCTURED(::tinywebserver::StructuredLogLevel::DEBUG, __VA_ARGS__)
#define LOG_S_INFO(...)  LOG_STRUCTURED(::tinywebserver::StructuredLogLevel::INFO, __VA_ARGS__)
#define LOG_S_WARN(...)  LOG_STRUCTURED(::tinywebserver::StructuredLogLevel::WARN, __VA_ARGS__)
#define LOG_S_ERROR(...) LOG_STRUCTURED(::tinywebserver::StructuredLogLevel::ERROR, __VA_ARGS__)
#define LOG_S_FATAL(...) LOG_STRUCTURED(::tinywebserver::StructuredLogLevel::FATAL, __VA_ARGS__)

// 请求上下文结构体
struct RequestContext {
    std::optional<std::string> request_id;
    std::optional<int> connection_id;
    std::optional<std::string> client_ip;
    std::optional<std::string> user_agent;
    std::optional<int> response_status;
};

} // namespace tinywebserver