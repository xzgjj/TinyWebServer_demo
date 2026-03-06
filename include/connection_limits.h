#pragma once

#include <cstddef>

/// 连接级资源限制配置
struct ConnectionLimits {
    /// 最大输入缓冲区大小（字节）
    static constexpr size_t kMaxInputBuffer = 64 * 1024;      // 64KB
    /// 最大输出缓冲区大小（字节）
    static constexpr size_t kMaxOutputBuffer = 1 * 1024 * 1024; // 1MB
    /// 单个HTTP请求最大大小（字节）
    static constexpr size_t kMaxRequestSize = 8 * 1024;       // 8KB
    /// HTTP头部最大大小（字节）
    static constexpr size_t kMaxHeadersSize = 8 * 1024;       // 8KB
    /// 单个连接最大并发请求数（用于长连接）
    static constexpr size_t kMaxRequestsPerConnection = 100;

    /// 检查输入缓冲区是否超过限制
    static bool IsInputBufferExceeded(size_t current_size) {
        return current_size > kMaxInputBuffer;
    }

    /// 检查输出缓冲区是否超过限制
    static bool IsOutputBufferExceeded(size_t current_size) {
        return current_size > kMaxOutputBuffer;
    }

    /// 检查请求大小是否超过限制
    static bool IsRequestSizeExceeded(size_t request_size) {
        return request_size > kMaxRequestSize;
    }

    /// 检查头部大小是否超过限制
    static bool IsHeadersSizeExceeded(size_t headers_size) {
        return headers_size > kMaxHeadersSize;
    }
};

/// 全局资源限制配置
struct GlobalLimits {
    /// 最大并发连接数
    static constexpr size_t kMaxConnections = 10000;
    /// 最大文件描述符数（系统级）
    static constexpr size_t kMaxFds = 10240;
    /// 服务器总内存限制（字节）
    static constexpr size_t kTotalMemoryLimit = 256 * 1024 * 1024; // 256MB
    /// 每个工作线程最大事件数
    static constexpr size_t kMaxEventsPerLoop = 10000;

    /// 检查连接数是否超过限制
    static bool IsConnectionLimitExceeded(size_t current_connections) {
        return current_connections > kMaxConnections;
    }

    /// 检查内存使用是否超过限制
    static bool IsMemoryLimitExceeded(size_t current_memory) {
        return current_memory > kTotalMemoryLimit;
    }
};