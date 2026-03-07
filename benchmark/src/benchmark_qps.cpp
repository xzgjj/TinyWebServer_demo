/**
 * @file benchmark_qps.cpp
 * @brief QPS (每秒查询数) 基准测试实现
 */

#include "../../include/benchmark.h"
#include "../../include/server.h"
#include "../../include/Logger.h"

#include <atomic>
#include <chrono>
#include <cstring>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <thread>
#include <unistd.h>
#include <vector>
#include <queue>
#include <condition_variable>
#include <random>
#include <arpa/inet.h>

namespace tinywebserver {
namespace benchmark {

/**
 * @brief 简单的HTTP客户端，用于发送请求并测量延迟
 */
class HttpClient {
public:
    struct RequestResult {
        bool success;
        int64_t latency_ms;  // 从发送到接收完整响应的延迟
        int64_t response_size;
        int status_code;      // HTTP状态码
        std::string error_message;
    };

    HttpClient(const std::string& host, int port, bool keep_alive = true)
        : host_(host), port_(port), keep_alive_(keep_alive), fd_(-1) {
        Connect();
    }

    ~HttpClient() {
        if (fd_ >= 0) {
            close(fd_);
        }
    }

    RequestResult SendRequest(const std::string& method, const std::string& path,
                             const std::string& body = "") {
        RequestResult result;
        result.success = false;

        // 确保连接有效
        if (fd_ < 0 && !Connect()) {
            result.error_message = "连接失败";
            return result;
        }

        // 构造HTTP请求
        std::string request = method + " " + path + " HTTP/1.1\r\n";
        request += "Host: " + host_ + "\r\n";
        if (!body.empty()) {
            request += "Content-Length: " + std::to_string(body.size()) + "\r\n";
        }
        if (!keep_alive_) {
            request += "Connection: close\r\n";
        } else {
            request += "Connection: keep-alive\r\n";
        }
        request += "\r\n";
        if (!body.empty()) {
            request += body;
        }

        // 发送请求并计时
        auto start_time = std::chrono::steady_clock::now();

        ssize_t sent = send(fd_, request.c_str(), request.size(), 0);
        if (sent < 0) {
            result.error_message = "发送失败: " + std::string(strerror(errno));
            CloseConnection();
            return result;
        }

        // 接收响应
        std::string response;
        char buffer[4096];

        // 设置接收超时
        struct timeval tv;
        tv.tv_sec = 5;
        tv.tv_usec = 0;
        setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        while (true) {
            ssize_t received = recv(fd_, buffer, sizeof(buffer), 0);
            if (received < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    // 超时
                    break;
                }
                result.error_message = "接收失败: " + std::string(strerror(errno));
                CloseConnection();
                return result;
            } else if (received == 0) {
                // 连接关闭
                if (!keep_alive_) {
                    CloseConnection();
                }
                break;
            } else {
                response.append(buffer, received);
                // 检查是否收到完整响应（简单的检查：包含\r\n\r\n）
                if (response.find("\r\n\r\n") != std::string::npos) {
                    // 如果有Content-Length，检查是否收到完整正文
                    size_t header_end = response.find("\r\n\r\n");
                    std::string headers = response.substr(0, header_end);
                    size_t content_length_pos = headers.find("Content-Length: ");
                    if (content_length_pos != std::string::npos) {
                        size_t cl_start = content_length_pos + 16;
                        size_t cl_end = headers.find("\r\n", cl_start);
                        std::string cl_str = headers.substr(cl_start, cl_end - cl_start);
                        int content_length = std::stoi(cl_str);
                        size_t body_received = response.size() - header_end - 4;
                        if (body_received >= static_cast<size_t>(content_length)) {
                            break; // 收到完整响应
                        }
                    } else {
                        // 没有Content-Length，假设响应完整
                        break;
                    }
                }
            }
        }

        auto end_time = std::chrono::steady_clock::now();
        result.latency_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            end_time - start_time).count();
        result.response_size = response.size();
        result.success = true;

        // 解析状态码
        if (response.size() >= 12) {
            // HTTP/1.1 200 OK\r\n
            std::string status_line = response.substr(0, response.find("\r\n"));
            if (status_line.size() >= 12 && status_line.substr(0, 5) == "HTTP/") {
                std::string status_code_str = status_line.substr(9, 3);
                try {
                    result.status_code = std::stoi(status_code_str);
                } catch (...) {
                    result.status_code = 0;
                }
            }
        }

        // 如果不是keep-alive或者连接有问题，关闭连接
        if (!keep_alive_ || fd_ < 0) {
            CloseConnection();
        }

        return result;
    }

private:
    bool Connect() {
        fd_ = socket(AF_INET, SOCK_STREAM, 0);
        if (fd_ < 0) {
            return false;
        }

        struct sockaddr_in addr;
        std::memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port_);

        if (inet_pton(AF_INET, host_.c_str(), &addr.sin_addr) <= 0) {
            close(fd_);
            fd_ = -1;
            return false;
        }

        // 设置连接超时
        struct timeval tv;
        tv.tv_sec = 3;
        tv.tv_usec = 0;
        setsockopt(fd_, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
        setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        if (connect(fd_, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
            close(fd_);
            fd_ = -1;
            return false;
        }

        return true;
    }

    void CloseConnection() {
        if (fd_ >= 0) {
            close(fd_);
            fd_ = -1;
        }
    }

    std::string host_;
    int port_;
    bool keep_alive_;
    int fd_;
};

/**
 * @brief QPS基准测试实现
 */
class QpsBenchmark : public Benchmark {
public:
    std::string GetName() const override {
        return "qps_benchmark";
    }

    std::string GetDescription() const override {
        return "测量服务器每秒处理请求数(QPS)和吞吐量";
    }

    BenchmarkResult Run(const BenchmarkConfig& config) override {
        BenchmarkResult result;
        result.name = GetName();
        result.start_time = std::chrono::system_clock::now();

        // 验证配置
        auto errors = config.Validate();
        if (!errors.empty()) {
            result.success = false;
            result.error_message = "配置验证失败: ";
            for (const auto& error : errors) {
                result.error_message += error + "; ";
            }
            result.end_time = std::chrono::system_clock::now();
            return result;
        }

        // 启动服务器（在独立线程中）
        std::unique_ptr<Server> server;
        std::thread server_thread;

        try {
            server = std::make_unique<Server>(config.server_host, config.server_port);
            server_thread = std::thread([&server]() {
                server->Start();
            });

            // 等待服务器启动
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            LOG_INFO("QPS基准测试: 服务器已启动在 %s:%d",
                    config.server_host.c_str(), config.server_port);
        } catch (const std::exception& e) {
            result.success = false;
            result.error_message = std::string("启动服务器失败: ") + e.what();
            result.end_time = std::chrono::system_clock::now();
            return result;
        }

        // 预热阶段
        WarmUp(config);

        // 运行基准测试
        std::atomic<int64_t> total_requests{0};
        std::atomic<int64_t> successful_requests{0};
        std::atomic<int64_t> failed_requests{0};
        std::atomic<int64_t> total_bytes_received{0};

        std::vector<double> latencies;
        std::mutex latencies_mutex;

        // 控制测试时间的标志
        std::atomic<bool> stop_test{false};
        auto test_start_time = std::chrono::steady_clock::now();

        // 工作线程函数
        auto worker_func = [&](int worker_id) {
            std::random_device rd;
            std::mt19937 gen(rd());

            while (!stop_test) {
                HttpClient client(config.server_host, config.server_port, config.keep_alive);

                auto result = client.SendRequest(config.request_method, config.request_path,
                                                config.request_body);

                total_requests++;

                if (result.success) {
                    successful_requests++;
                    total_bytes_received += result.response_size;

                    std::lock_guard<std::mutex> lock(latencies_mutex);
                    latencies.push_back(static_cast<double>(result.latency_ms));
                } else {
                    failed_requests++;
                }

                // 如果设置了目标QPS，控制发送速率
                if (config.target_qps > 0) {
                    int64_t requests_per_worker = config.target_qps / config.concurrent_connections;
                    if (requests_per_worker > 0) {
                        std::this_thread::sleep_for(std::chrono::milliseconds(1000 / requests_per_worker));
                    }
                }
            }
        };

        // 启动工作线程
        std::vector<std::thread> workers;
        for (int i = 0; i < config.concurrent_connections; ++i) {
            workers.emplace_back(worker_func, i);
        }

        // 运行指定时间
        std::this_thread::sleep_for(std::chrono::duration<double>(config.duration_seconds));

        // 停止测试
        stop_test = true;

        // 等待所有工作线程完成
        for (auto& worker : workers) {
            if (worker.joinable()) {
                worker.join();
            }
        }

        auto test_end_time = std::chrono::steady_clock::now();
        double test_duration = std::chrono::duration<double>(test_end_time - test_start_time).count();

        // 停止服务器
        if (server) {
            server->Stop();
        }
        if (server_thread.joinable()) {
            server_thread.join();
        }

        // 计算结果指标
        double qps = (test_duration > 0) ? successful_requests / test_duration : 0;
        double throughput_mbps = (test_duration > 0) ?
            (total_bytes_received * 8.0 / (1024 * 1024)) / test_duration : 0;
        double error_rate = (total_requests > 0) ?
            static_cast<double>(failed_requests) / total_requests : 0;

        // 计算延迟统计
        double avg_latency = 0;
        double p50_latency = 0;
        double p90_latency = 0;
        double p99_latency = 0;

        if (!latencies.empty()) {
            std::sort(latencies.begin(), latencies.end());

            double sum_latency = 0;
            for (double latency : latencies) {
                sum_latency += latency;
            }
            avg_latency = sum_latency / latencies.size();

            p50_latency = Statistics::CalculatePercentile(latencies, 50.0);
            p90_latency = Statistics::CalculatePercentile(latencies, 90.0);
            p99_latency = Statistics::CalculatePercentile(latencies, 99.0);
        }

        // 填充结果
        result.success = true;
        result.duration_seconds = test_duration;
        result.end_time = std::chrono::system_clock::now();

        // 添加指标
        result.metrics.push_back({"qps", qps, "requests/second", "每秒查询数"});
        result.metrics.push_back({"throughput_mbps", throughput_mbps, "Mbps", "网络吞吐量"});
        result.metrics.push_back({"total_requests", static_cast<double>(total_requests), "requests", "总请求数"});
        result.metrics.push_back({"successful_requests", static_cast<double>(successful_requests), "requests", "成功请求数"});
        result.metrics.push_back({"failed_requests", static_cast<double>(failed_requests), "requests", "失败请求数"});
        result.metrics.push_back({"error_rate", error_rate * 100, "%", "错误率"});
        result.metrics.push_back({"avg_latency", avg_latency, "ms", "平均延迟"});
        result.metrics.push_back({"p50_latency", p50_latency, "ms", "50%分位延迟"});
        result.metrics.push_back({"p90_latency", p90_latency, "ms", "90%分位延迟"});
        result.metrics.push_back({"p99_latency", p99_latency, "ms", "99%分位延迟"});
        result.metrics.push_back({"concurrent_connections", static_cast<double>(config.concurrent_connections), "connections", "并发连接数"});

        // 清理阶段
        CleanUp(config);

        LOG_INFO("QPS基准测试完成: QPS=%.2f, 延迟P99=%.2fms", qps, p99_latency);

        return result;
    }

    void WarmUp(const BenchmarkConfig& config) override {
        // 预热：发送一些请求让服务器进入稳定状态
        LOG_INFO("QPS基准测试: 预热阶段开始");

        const int warmup_requests = 100;
        std::vector<std::thread> warmup_threads;

        for (int i = 0; i < std::min(10, config.concurrent_connections); ++i) {
            warmup_threads.emplace_back([&config, i]() {
                HttpClient client(config.server_host, config.server_port, config.keep_alive);
                for (int j = 0; j < warmup_requests / 10; ++j) {
                    client.SendRequest(config.request_method, config.request_path, config.request_body);
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                }
            });
        }

        for (auto& thread : warmup_threads) {
            if (thread.joinable()) {
                thread.join();
            }
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        LOG_INFO("QPS基准测试: 预热阶段完成");
    }

    void CleanUp(const BenchmarkConfig& /*config*/) override {
        // 清理资源
        LOG_INFO("QPS基准测试: 清理完成");
    }
};

// 工厂函数
std::unique_ptr<Benchmark> CreateQpsBenchmark() {
    return std::make_unique<QpsBenchmark>();
}

} // namespace benchmark
} // namespace tinywebserver