#include "error/error.h"

#include <cstring>
#include <iostream>
#include <sstream>

namespace tinywebserver {

namespace {

const char* GetErrorCodeString(WebError code) noexcept {
    switch (code) {
        case WebError::kSuccess:
            return "Success";

        // 系统相关错误
        case WebError::kSocketError:
            return "Socket operation failed";
        case WebError::kEpollError:
            return "Epoll operation failed";
        case WebError::kFcntlError:
            return "Fcntl operation failed";
        case WebError::kIoError:
            return "I/O error";
        case WebError::kFileNotFound:
            return "File not found";
        case WebError::kPermissionDenied:
            return "Permission denied";

        // 网络协议错误
        case WebError::kParseError:
            return "Parse error";
        case WebError::kInvalidRequest:
            return "Invalid request";
        case WebError::kUnsupportedMethod:
            return "Unsupported HTTP method";
        case WebError::kProtocolError:
            return "Protocol error";
        case WebError::kRequestTimeout:
            return "Request timeout";

        // 资源限制错误
        case WebError::kBufferFull:
            return "Buffer full";
        case WebError::kConnectionLimit:
            return "Connection limit exceeded";
        case WebError::kMemoryLimit:
            return "Memory limit exceeded";
        case WebError::kQueueFull:
            return "Queue full";
        case WebError::kResourceExhausted:
            return "Resource exhausted";

        // 安全相关错误
        case WebError::kAccessDenied:
            return "Access denied";
        case WebError::kInvalidPath:
            return "Invalid path";
        case WebError::kRequestTooLarge:
            return "Request too large";

        // 内部逻辑错误
        case WebError::kInternalError:
            return "Internal error";
        case WebError::kStateError:
            return "State machine error";
        case WebError::kNullPointer:
            return "Null pointer access";
        case WebError::kInvalidArgument:
            return "Invalid argument";

        // 定时器相关错误
        case WebError::kTimerError:
            return "Timer operation failed";
        case WebError::kTimeout:
            return "Timeout";

        // 未知错误
        case WebError::kUnknownError:
            return "Unknown error";

        default:
            return "Unrecognized error code";
    }
}

bool IsRetriableErrno(int err) noexcept {
    switch (err) {
        case EAGAIN:
#if EAGAIN != EWOULDBLOCK
        case EWOULDBLOCK:
#endif
        case EINTR:
        case ECONNRESET:
        case ETIMEDOUT:
            return true;
        default:
            return false;
    }
}

bool IsConnectionClosedErrno(int err) noexcept {
    switch (err) {
        case ECONNRESET:
        case EPIPE:
        case ECONNABORTED:
        case ENOTCONN:
        case ETIMEDOUT:
            return true;
        default:
            return false;
    }
}

} // 匿名命名空间

// ============================================================================
// Error 类实现
// ============================================================================

Error::Error() noexcept
    : code_(WebError::kSuccess),
      message_("Success"),
      sys_errno_(0),
      timestamp_(std::chrono::system_clock::now()) {
}

Error::Error(WebError code, std::string message, int sys_errno) noexcept
    : code_(code),
      message_(std::move(message)),
      sys_errno_(sys_errno),
      timestamp_(std::chrono::system_clock::now()) {

    // 如果消息为空，使用错误码的默认描述
    if (message_.empty()) {
        message_ = GetErrorCodeString(code_);
    }
}

Error::Error(int sys_errno, std::string message) noexcept
    : code_(WebError::kUnknownError),
      message_(std::move(message)),
      sys_errno_(sys_errno),
      timestamp_(std::chrono::system_clock::now()) {

    // 根据系统错误号推断项目错误码
    if (sys_errno_ == 0) {
        code_ = WebError::kSuccess;
        if (message_.empty()) {
            message_ = "Success";
        }
    } else {
        // 映射常见系统错误到项目错误码
        switch (sys_errno_) {
            case EACCES:
            case EPERM:
                code_ = WebError::kPermissionDenied;
                break;
            case ENOENT:
                code_ = WebError::kFileNotFound;
                break;
            case ENOMEM:
                code_ = WebError::kMemoryLimit;
                break;
            case ENFILE:
            case EMFILE:
                code_ = WebError::kConnectionLimit;
                break;
            case EINVAL:
                code_ = WebError::kInvalidArgument;
                break;
            case ETIMEDOUT:
                code_ = WebError::kTimeout;
                break;
            case EBADF:
            case ENOTSOCK:
                code_ = WebError::kSocketError;
                break;
            case EAGAIN:
#if EAGAIN != EWOULDBLOCK
            case EWOULDBLOCK:
#endif
                code_ = WebError::kBufferFull;
                break;
            default:
                code_ = WebError::kUnknownError;
                break;
        }

        // 如果消息为空，使用系统错误描述
        if (message_.empty()) {
            message_ = std::strerror(sys_errno_);
        }
    }
}

std::string Error::ToString() const {
    std::ostringstream oss;

    // 添加时间戳（可选）
    // auto time_t = std::chrono::system_clock::to_time_t(timestamp_);
    // oss << std::put_time(std::localtime(&time_t), "%Y-%m-%d %H:%M:%S") << " ";

    // 添加错误码
    oss << "[" << static_cast<int>(code_) << "] " << GetErrorCodeString(code_);

    // 添加自定义消息
    if (!message_.empty() && message_ != GetErrorCodeString(code_)) {
        oss << ": " << message_;
    }

    // 添加系统错误号和信息
    if (sys_errno_ != 0) {
        oss << " (errno=" << sys_errno_ << ": " << std::strerror(sys_errno_) << ")";
    }

    return oss.str();
}

std::error_code Error::ToSystemError() const {
    if (sys_errno_ != 0) {
        return std::error_code(sys_errno_, std::generic_category());
    }

    // 如果没有系统错误号，映射项目错误码到最接近的系统错误
    switch (code_) {
        case WebError::kPermissionDenied:
            return std::error_code(EACCES, std::generic_category());
        case WebError::kFileNotFound:
            return std::error_code(ENOENT, std::generic_category());
        case WebError::kMemoryLimit:
            return std::error_code(ENOMEM, std::generic_category());
        case WebError::kTimeout:
            return std::error_code(ETIMEDOUT, std::generic_category());
        case WebError::kInvalidArgument:
            return std::error_code(EINVAL, std::generic_category());
        default:
            return std::error_code(EIO, std::generic_category());
    }
}

Error& Error::AddContext(const std::string& context) {
    if (!context.empty()) {
        if (!message_.empty()) {
            message_ = context + ": " + message_;
        } else {
            message_ = context;
        }
    }
    return *this;
}

Error Error::Success() noexcept {
    return Error();
}

Error Error::FromErrno(int sys_errno, std::string message) noexcept {
    return Error(sys_errno, std::move(message));
}

Error Error::FromException(const std::exception& e) noexcept {
    return Error(WebError::kInternalError, e.what());
}

std::string Error::ErrorCodeToString(WebError code) noexcept {
    return GetErrorCodeString(code);
}

bool Error::IsRetriableError(int sys_errno) noexcept {
    return IsRetriableErrno(sys_errno);
}

bool Error::IsConnectionClosedError(int sys_errno) noexcept {
    return IsConnectionClosedErrno(sys_errno);
}

// ============================================================================
// 辅助函数实现
// ============================================================================

std::ostream& operator<<(std::ostream& os, const Error& error) {
    os << error.ToString();
    return os;
}

bool operator==(const Error& lhs, const Error& rhs) noexcept {
    return lhs.GetCode() == rhs.GetCode() &&
           lhs.GetSysErrno() == rhs.GetSysErrno() &&
           lhs.GetMessage() == rhs.GetMessage();
}

bool operator!=(const Error& lhs, const Error& rhs) noexcept {
    return !(lhs == rhs);
}

} // namespace tinywebserver