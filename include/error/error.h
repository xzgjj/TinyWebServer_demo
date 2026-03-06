#pragma once

#include <chrono>
#include <cstdint>
#include <string>
#include <system_error>

/**
 * @file error.h
 * @brief 统一错误处理系统
 *
 * 提供项目级错误码枚举和错误包装类，兼容系统 errno。
 * 所有错误路径必须使用此模块进行错误报告。
 */

namespace tinywebserver {

/**
 * @brief 项目级错误码枚举
 *
 * 定义项目内部使用的错误码，与系统 errno 兼容。
 * 错误码值从 1000 开始，避免与系统 errno 冲突。
 */
enum class WebError : int32_t {
    /// 操作成功
    kSuccess = 0,

    // 系统相关错误 (1000-1099)
    kSocketError = 1000,           ///< socket 系统调用失败
    kEpollError = 1001,            ///< epoll 操作失败
    kFcntlError = 1002,            ///< fcntl 操作失败
    kIoError = 1003,               ///< 通用 I/O 错误
    kFileNotFound = 1004,          ///< 文件未找到
    kPermissionDenied = 1005,      ///< 权限拒绝

    // 网络协议错误 (1100-1199)
    kParseError = 1100,            ///< HTTP 解析失败
    kInvalidRequest = 1101,        ///< 无效的 HTTP 请求
    kUnsupportedMethod = 1102,     ///< 不支持的 HTTP 方法
    kProtocolError = 1103,         ///< 协议错误
    kRequestTimeout = 1104,        ///< 请求超时

    // 资源限制错误 (1200-1299)
    kBufferFull = 1200,            ///< 缓冲区已满
    kConnectionLimit = 1201,       ///< 连接数超限
    kMemoryLimit = 1202,           ///< 内存不足
    kQueueFull = 1203,             ///< 队列已满
    kResourceExhausted = 1204,     ///< 资源耗尽

    // 安全相关错误 (1300-1399)
    kAccessDenied = 1300,          ///< 访问被拒绝
    kInvalidPath = 1301,           ///< 无效路径（路径遍历攻击尝试）
    kRequestTooLarge = 1302,       ///< 请求体过大

    // 内部逻辑错误 (1400-1499)
    kInternalError = 1400,         ///< 内部逻辑错误
    kStateError = 1401,            ///< 状态机状态错误
    kNullPointer = 1402,           ///< 空指针访问
    kInvalidArgument = 1403,       ///< 无效参数

    // 定时器相关错误 (1500-1599)
    kTimerError = 1500,            ///< 定时器操作失败
    kTimeout = 1501,               ///< 操作超时

    // 未知错误
    kUnknownError = 9999           ///< 未知错误
};

/**
 * @brief 错误包装类
 *
 * 封装错误码、错误消息、系统 errno 和时间戳。
 * 提供链式错误构建和错误传播支持。
 */
class Error {
public:
    /**
     * @brief 默认构造函数，创建成功错误对象
     */
    Error() noexcept;

    /**
     * @brief 从项目错误码构造
     * @param code 项目错误码
     * @param message 错误描述信息
     * @param sys_errno 系统错误号（默认为0）
     */
    Error(WebError code, std::string message, int sys_errno = 0) noexcept;

    /**
     * @brief 从系统错误号构造
     * @param sys_errno 系统错误号
     * @param message 错误描述信息（可选）
     */
    explicit Error(int sys_errno, std::string message = "") noexcept;

    /**
     * @brief 拷贝构造函数
     */
    Error(const Error&) = default;

    /**
     * @brief 移动构造函数
     */
    Error(Error&&) = default;

    /**
     * @brief 拷贝赋值运算符
     */
    Error& operator=(const Error&) = default;

    /**
     * @brief 移动赋值运算符
     */
    Error& operator=(Error&&) = default;

    /**
     * @brief 析构函数
     */
    ~Error() = default;

    /**
     * @brief 检查错误是否成功
     * @return true 表示成功，false 表示失败
     */
    bool IsSuccess() const noexcept { return code_ == WebError::kSuccess; }

    /**
     * @brief 检查错误是否失败
     * @return true 表示失败，false 表示成功
     */
    bool IsFailure() const noexcept { return !IsSuccess(); }

    /**
     * @brief 获取项目错误码
     * @return WebError 枚举值
     */
    WebError GetCode() const noexcept { return code_; }

    /**
     * @brief 获取系统错误号
     * @return 系统错误号，如果没有系统错误则返回0
     */
    int GetSysErrno() const noexcept { return sys_errno_; }

    /**
     * @brief 获取错误描述信息
     * @return 错误描述字符串
     */
    const std::string& GetMessage() const noexcept { return message_; }

    /**
     * @brief 获取错误时间戳
     * @return 错误发生的时间点
     */
    std::chrono::system_clock::time_point GetTimestamp() const noexcept {
        return timestamp_;
    }

    /**
     * @brief 转换为可读字符串
     * @return 格式化的错误字符串
     */
    std::string ToString() const;

    /**
     * @brief 转换为系统错误类别
     * @return std::error_code 对象
     */
    std::error_code ToSystemError() const;

    /**
     * @brief 添加错误上下文信息
     * @param context 上下文描述
     * @return 当前错误对象的引用，支持链式调用
     */
    Error& AddContext(const std::string& context);

    /**
     * @brief 创建成功错误对象（静态工厂方法）
     * @return 表示成功的错误对象
     */
    static Error Success() noexcept;

    /**
     * @brief 从系统错误号创建错误对象（静态工厂方法）
     * @param sys_errno 系统错误号
     * @return 错误对象
     */
    static Error FromErrno(int sys_errno, std::string message = "") noexcept;

    /**
     * @brief 从异常创建错误对象（静态工厂方法）
     * @param e 标准异常引用
     * @return 错误对象
     */
    static Error FromException(const std::exception& e) noexcept;

    /**
     * @brief 获取错误码的描述字符串
     * @param code 错误码
     * @return 错误码描述
     */
    static std::string ErrorCodeToString(WebError code) noexcept;

    /**
     * @brief 检查系统错误号是否表示需要重试
     * @param sys_errno 系统错误号
     * @return true 表示应重试操作
     */
    static bool IsRetriableError(int sys_errno) noexcept;

    /**
     * @brief 检查错误是否表示连接已关闭
     * @param sys_errno 系统错误号
     * @return true 表示连接已关闭
     */
    static bool IsConnectionClosedError(int sys_errno) noexcept;

private:
    WebError code_;
    std::string message_;
    int sys_errno_;
    std::chrono::system_clock::time_point timestamp_;
};

/**
 * @brief 错误流输出运算符
 * @param os 输出流
 * @param error 错误对象
 * @return 输出流引用
 */
std::ostream& operator<<(std::ostream& os, const Error& error);

/**
 * @brief 错误比较运算符
 */
bool operator==(const Error& lhs, const Error& rhs) noexcept;
bool operator!=(const Error& lhs, const Error& rhs) noexcept;

// ============================================================================
// 错误处理辅助宏
// ============================================================================

/**
 * @brief 如果表达式返回错误则返回该错误
 *
 * 用法：RETURN_IF_ERROR(SomeFunctionThatReturnsError());
 */
#define RETURN_IF_ERROR(expr) \
    do { \
        auto _result = (expr); \
        if (_result.IsFailure()) return _result; \
    } while (0)

/**
 * @brief 如果表达式返回错误则返回该错误，并添加上下文
 *
 * 用法：RETURN_IF_ERROR_CTX(SomeFunction(), "While processing request");
 */
#define RETURN_IF_ERROR_CTX(expr, context) \
    do { \
        auto _result = (expr); \
        if (_result.IsFailure()) return _result.AddContext(context); \
    } while (0)

/**
 * @brief 如果条件为真则返回错误
 *
 * 用法：RETURN_ERROR_IF(buffer.empty(), WebError::kBufferFull, "Buffer is empty");
 */
#define RETURN_ERROR_IF(condition, error_code, ...) \
    do { \
        if (condition) return Error(error_code, ##__VA_ARGS__); \
    } while (0)

/**
 * @brief 如果系统调用失败则返回错误
 *
 * 用法：RETURN_ON_SYSERROR(ret == -1, "socket");
 */
#define RETURN_ON_SYSERROR(condition, syscall_name) \
    do { \
        if (condition) return Error::FromErrno(errno, std::string("System call failed: ") + syscall_name); \
    } while (0)

/**
 * @brief 尝试执行操作，如果失败则记录日志并返回错误
 *
 * 用法：TRY_WITH_LOG(CreateConnection(), "Failed to create connection");
 */
#define TRY_WITH_LOG(expr, log_message) \
    do { \
        auto _result = (expr); \
        if (_result.IsFailure()) { \
            LOG_ERROR << log_message << ": " << _result.ToString(); \
            return _result; \
        } \
    } while (0)

/**
 * @brief 忽略错误（用于明确忽略预期中的错误）
 *
 * 用法：IGNORE_ERROR(CloseConnection());
 */
#define IGNORE_ERROR(expr) \
    do { \
        auto _result = (expr); \
        if (_result.IsFailure()) { \
            /* 明确忽略错误 */ \
        } \
    } while (0)

} // namespace tinywebserver