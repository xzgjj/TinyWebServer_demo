#include "server_metrics.h"
#include <sstream>
#include <iomanip>

namespace tinywebserver {

std::string ServerMetrics::ToJsonString() const {
    auto snapshot = GetSnapshot();
    std::ostringstream oss;
    oss << "{";
    oss << "\"connections\": {";
    oss << "\"active\": " << snapshot.active_connections << ", ";
    oss << "\"total\": " << snapshot.total_connections;
    oss << "}, ";
    oss << "\"requests\": {";
    oss << "\"total\": " << snapshot.total_requests << ", ";
    oss << "\"1xx\": " << snapshot.requests_1xx << ", ";
    oss << "\"2xx\": " << snapshot.requests_2xx << ", ";
    oss << "\"3xx\": " << snapshot.requests_3xx << ", ";
    oss << "\"4xx\": " << snapshot.requests_4xx << ", ";
    oss << "\"5xx\": " << snapshot.requests_5xx;
    oss << "}, ";
    oss << "\"traffic\": {";
    oss << "\"bytes_sent\": " << snapshot.bytes_sent << ", ";
    oss << "\"bytes_received\": " << snapshot.bytes_received;
    oss << "}, ";
    oss << "\"errors\": " << snapshot.errors << ", ";
    oss << "\"resources\": {";
    oss << "\"memory_allocated\": " << snapshot.memory_allocated << ", ";
    oss << "\"epoll_wait_time_us\": " << snapshot.epoll_wait_time_us;
    oss << "}, ";
    oss << "\"uptime_seconds\": " << std::fixed << std::setprecision(2) << snapshot.uptime_sec;
    oss << "}";
    return oss.str();
}

std::string ServerMetrics::ToPrometheusString() const {
    auto snapshot = GetSnapshot();
    std::ostringstream oss;

    // Prometheus 格式要求：指标名 {标签} 值
    // 使用下划线命名法
    oss << "# HELP webserver_connections_active Current active connections\n";
    oss << "# TYPE webserver_connections_active gauge\n";
    oss << "webserver_connections_active " << snapshot.active_connections << "\n\n";

    oss << "# HELP webserver_connections_total Total connections accepted\n";
    oss << "# TYPE webserver_connections_total counter\n";
    oss << "webserver_connections_total " << snapshot.total_connections << "\n\n";

    oss << "# HELP webserver_requests_total Total requests processed\n";
    oss << "# TYPE webserver_requests_total counter\n";
    oss << "webserver_requests_total " << snapshot.total_requests << "\n\n";

    oss << "# HELP webserver_requests_by_status_total Total requests by status code family\n";
    oss << "# TYPE webserver_requests_by_status_total counter\n";
    oss << "webserver_requests_by_status_total{status=\"1xx\"} " << snapshot.requests_1xx << "\n";
    oss << "webserver_requests_by_status_total{status=\"2xx\"} " << snapshot.requests_2xx << "\n";
    oss << "webserver_requests_by_status_total{status=\"3xx\"} " << snapshot.requests_3xx << "\n";
    oss << "webserver_requests_by_status_total{status=\"4xx\"} " << snapshot.requests_4xx << "\n";
    oss << "webserver_requests_by_status_total{status=\"5xx\"} " << snapshot.requests_5xx << "\n\n";

    oss << "# HELP webserver_traffic_bytes_sent_total Total bytes sent\n";
    oss << "# TYPE webserver_traffic_bytes_sent_total counter\n";
    oss << "webserver_traffic_bytes_sent_total " << snapshot.bytes_sent << "\n\n";

    oss << "# HELP webserver_traffic_bytes_received_total Total bytes received\n";
    oss << "# TYPE webserver_traffic_bytes_received_total counter\n";
    oss << "webserver_traffic_bytes_received_total " << snapshot.bytes_received << "\n\n";

    oss << "# HELP webserver_errors_total Total errors encountered\n";
    oss << "# TYPE webserver_errors_total counter\n";
    oss << "webserver_errors_total " << snapshot.errors << "\n\n";

    oss << "# HELP webserver_memory_allocated_bytes Current allocated memory in bytes\n";
    oss << "# TYPE webserver_memory_allocated_bytes gauge\n";
    oss << "webserver_memory_allocated_bytes " << snapshot.memory_allocated << "\n\n";

    oss << "# HELP webserver_epoll_wait_time_microseconds_total Total time spent in epoll_wait\n";
    oss << "# TYPE webserver_epoll_wait_time_microseconds_total counter\n";
    oss << "webserver_epoll_wait_time_microseconds_total " << snapshot.epoll_wait_time_us << "\n\n";

    oss << "# HELP webserver_uptime_seconds Server uptime in seconds\n";
    oss << "# TYPE webserver_uptime_seconds gauge\n";
    oss << "webserver_uptime_seconds " << std::fixed << std::setprecision(2) << snapshot.uptime_sec << "\n";

    return oss.str();
}

void ServerMetrics::Reset() {
    // 重置所有指标
    active_connections_ = 0;
    total_connections_ = 0;
    total_requests_ = 0;
    requests_1xx_ = 0;
    requests_2xx_ = 0;
    requests_3xx_ = 0;
    requests_4xx_ = 0;
    requests_5xx_ = 0;
    total_bytes_sent_ = 0;
    total_bytes_received_ = 0;
    error_count_ = 0;
    memory_allocated_ = 0;
    epoll_wait_time_us_ = 0;
    start_time_ = std::chrono::steady_clock::now();
}

} // namespace tinywebserver