// 指标与诊断

#ifndef SERVER_METRICS_H
#define SERVER_METRICS_H

#include <atomic>
#include <string>
#include <chrono>
#include <vector>
#include <map>

namespace tinywebserver {

/**
 * @brief 全局服务器指标监控类 (单例)
 * 符合功能安全要求，提供实时状态查询接口
 */
class ServerMetrics
{
public:
    static ServerMetrics& GetInstance()
    {
        static ServerMetrics instance;
        return instance;
    }

    // ==================== 指标记录接口 ====================

    // 连接层指标
    void OnNewConnection() {
        active_connections_++;
        total_connections_++;
    }
    void OnCloseConnection() {
        active_connections_--;
    }

    // 请求层指标
    void OnRequest() {
        total_requests_++;
    }
    void OnRequestWithStatusCode(int status_code) {
        total_requests_++;
        if (status_code >= 100 && status_code < 200) {
            requests_1xx_++;
        } else if (status_code >= 200 && status_code < 300) {
            requests_2xx_++;
        } else if (status_code >= 300 && status_code < 400) {
            requests_3xx_++;
        } else if (status_code >= 400 && status_code < 500) {
            requests_4xx_++;
        } else if (status_code >= 500 && status_code < 600) {
            requests_5xx_++;
        }
    }

    // 流量指标
    void OnBytesSent(size_t bytes) {
        total_bytes_sent_ += bytes;
    }
    void OnBytesReceived(size_t bytes) {
        total_bytes_received_ += bytes;
    }

    // 错误指标
    void OnError() {
        error_count_++;
    }

    // 资源指标
    void OnMemoryAllocated(size_t bytes) {
        memory_allocated_ += bytes;
    }
    void OnMemoryFreed(size_t bytes) {
        memory_allocated_ -= bytes;
    }

    // 性能指标
    void OnEpollWaitTime(uint64_t microseconds) {
        epoll_wait_time_us_ += microseconds;
    }

    // ==================== 状态查询接口 ====================

    struct Snapshot
    {
        // 连接指标
        uint64_t active_connections;
        uint64_t total_connections;

        // 请求指标
        uint64_t total_requests;
        uint64_t requests_1xx;
        uint64_t requests_2xx;
        uint64_t requests_3xx;
        uint64_t requests_4xx;
        uint64_t requests_5xx;

        // 流量指标
        uint64_t bytes_sent;
        uint64_t bytes_received;

        // 错误指标
        uint64_t errors;

        // 资源指标
        int64_t memory_allocated;
        uint64_t epoll_wait_time_us;

        // 运行时信息
        double uptime_sec;
    };

    Snapshot GetSnapshot() const
    {
        auto now = std::chrono::steady_clock::now();
        std::chrono::duration<double> diff = now - start_time_;

        return {
            active_connections_.load(),
            total_connections_.load(),
            total_requests_.load(),
            requests_1xx_.load(),
            requests_2xx_.load(),
            requests_3xx_.load(),
            requests_4xx_.load(),
            requests_5xx_.load(),
            total_bytes_sent_.load(),
            total_bytes_received_.load(),
            error_count_.load(),
            memory_allocated_.load(),
            epoll_wait_time_us_.load(),
            diff.count()
        };
    }

    // ==================== 导出接口 ====================

    /**
     * @brief 转换为 JSON 字符串
     */
    std::string ToJsonString() const;

    /**
     * @brief 转换为 Prometheus 格式字符串
     */
    std::string ToPrometheusString() const;

    /**
     * @brief 重置所有指标（用于测试）
     */
    void Reset();

private:
    ServerMetrics() : start_time_(std::chrono::steady_clock::now()) {}

    // 禁止拷贝
    ServerMetrics(const ServerMetrics&) = delete;
    ServerMetrics& operator=(const ServerMetrics&) = delete;

    // 运行时信息
    std::chrono::steady_clock::time_point start_time_;

    // 连接指标
    std::atomic<uint64_t> active_connections_{0};
    std::atomic<uint64_t> total_connections_{0};

    // 请求指标
    std::atomic<uint64_t> total_requests_{0};
    std::atomic<uint64_t> requests_1xx_{0};
    std::atomic<uint64_t> requests_2xx_{0};
    std::atomic<uint64_t> requests_3xx_{0};
    std::atomic<uint64_t> requests_4xx_{0};
    std::atomic<uint64_t> requests_5xx_{0};

    // 流量指标
    std::atomic<uint64_t> total_bytes_sent_{0};
    std::atomic<uint64_t> total_bytes_received_{0};

    // 错误指标
    std::atomic<uint64_t> error_count_{0};

    // 资源指标
    std::atomic<int64_t> memory_allocated_{0};  // 可能为负（如果释放多于分配）
    std::atomic<uint64_t> epoll_wait_time_us_{0};
};

} // namespace tinywebserver

#endif