#pragma once

#include <unordered_map>
#include <mutex>
#include <chrono>
#include <string>
#include <atomic>
#include <vector>
#include "error/error.h"

namespace tinywebserver {

/**
 * @brief Keep-Alive 连接管理器
 *
 * 管理 HTTP/1.1 持久连接，跟踪请求计数，处理空闲超时。
 * 线程安全，可在多个连接间共享。
 */
class KeepAliveManager {
public:
    struct ConnectionState {
        int request_count;                             ///< 当前连接上的请求计数
        std::chrono::steady_clock::time_point last_active; ///< 最后活动时间
        bool keep_alive;                               ///< 是否保持连接
        std::chrono::seconds idle_timeout;             ///< 空闲超时时间
    };

    /**
     * @brief 构造函数
     * @param default_idle_timeout 默认空闲超时（秒）
     */
    explicit KeepAliveManager(std::chrono::seconds default_idle_timeout = std::chrono::seconds(15));

    /**
     * @brief 析构函数
     */
    ~KeepAliveManager() = default;

    /**
     * @brief 开始处理新请求
     * @param fd 连接文件描述符
     * @param keep_alive 是否启用 Keep-Alive
     * @param idle_timeout 空闲超时时间（秒），0表示使用默认值
     */
    void OnRequestStart(int fd, bool keep_alive, int idle_timeout = 0);

    /**
     * @brief 请求处理完成
     * @param fd 连接文件描述符
     */
    void OnRequestComplete(int fd);

    /**
     * @brief 连接关闭
     * @param fd 连接文件描述符
     */
    void OnConnectionClose(int fd);

    /**
     * @brief 检查连接是否空闲超时
     * @param fd 连接文件描述符
     * @return true 如果连接已空闲超时
     */
    bool IsIdleTimeout(int fd) const;

    /**
     * @brief 获取连接的空闲时间
     * @param fd 连接文件描述符
     * @return 空闲时间（秒），如果连接不存在返回-1
     */
    int GetIdleSeconds(int fd) const;

    /**
     * @brief 获取连接状态
     * @param fd 连接文件描述符
     * @return 连接状态，如果连接不存在返回nullptr
     */
    const ConnectionState* GetConnectionState(int fd) const;

    /**
     * @brief 清理所有超时连接
     * @return 被清理的连接fd列表
     */
    std::vector<int> CleanupTimeoutConnections();

    /**
     * @brief 获取活跃连接数
     */
    size_t GetActiveConnectionCount() const;

    /**
     * @brief 获取总请求数
     */
    int64_t GetTotalRequestCount() const;

    /**
     * @brief 重置统计信息（主要用于测试）
     */
    void ResetStatistics();

private:
    mutable std::mutex mutex_;
    std::unordered_map<int, ConnectionState> connections_;
    std::chrono::seconds default_idle_timeout_;

    std::atomic<int64_t> total_requests_{0};
    std::atomic<int64_t> total_timeout_closures_{0};
};

} // namespace tinywebserver