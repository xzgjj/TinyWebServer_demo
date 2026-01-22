// 指标与诊断

#ifndef SERVER_METRICS_H
#define SERVER_METRICS_H

#include <atomic>
#include <string>
#include <chrono>

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

    // 指标记录
    void OnNewConnection() { active_connections_++; total_connections_++; }
    void OnCloseConnection() { active_connections_--; }
    void OnRequest() { total_requests_++; }
    void OnBytesSent(size_t bytes) { total_bytes_sent_ += bytes; }
    void OnError() { error_count_++; }

    // 状态查询接口
    struct Snapshot
    {
        uint64_t active_conn;
        uint64_t total_conn;
        uint64_t total_req;
        uint64_t bytes_sent;
        uint64_t errors;
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
            total_bytes_sent_.load(),
            error_count_.load(),
            diff.count()
        };
    }

    std::string ToJsonString() const;

private:
    ServerMetrics() : start_time_(std::chrono::steady_clock::now()) {}
    
    std::chrono::steady_clock::time_point start_time_;
    std::atomic<uint64_t> active_connections_{0};
    std::atomic<uint64_t> total_connections_{0};
    std::atomic<uint64_t> total_requests_{0};
    std::atomic<uint64_t> total_bytes_sent_{0};
    std::atomic<uint64_t> error_count_{0};
};

#endif