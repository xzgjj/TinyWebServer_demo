/**
 * @file benchmark_latency.cpp
 * @brief 延迟分布基准测试实现
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
 * @brief 延迟基准测试实现
 *
 * 专注于测量请求延迟分布，特别是尾部延迟（P90, P99, P99.9）
 */
class LatencyBenchmark : public Benchmark {
public:
    std::string GetName() const override {
        return "latency_benchmark";
    }

    std::string GetDescription() const override {
        return "测量请求延迟分布，特别关注尾部延迟";
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

        // 启动服务器
        std::unique_ptr<Server> server;
        std::thread server_thread;

        try {
            server = std::make_unique<Server>(config.server_host, config.server_port, PluginManager::GetInstance());
            server_thread = std::thread([&server]() {
                server->Run();
            });

            std::this_thread::sleep_for(std::chrono::milliseconds(2000));
            LOG_INFO("延迟基准测试: 服务器已启动");
        } catch (const std::exception& e) {
            result.success = false;
            result.error_message = std::string("启动服务器失败: ") + e.what();
            result.end_time = std::chrono::system_clock::now();
            return result;
        }

        // 预热
        WarmUp(config);

        // 运行测试
        std::atomic<int64_t> total_requests{0};
        std::atomic<int64_t> successful_requests{0};
        std::atomic<int64_t> failed_requests{0};
        std::vector<double> all_latencies;
        std::mutex latencies_mutex;

        // 使用固定请求速率，避免队列堆积影响延迟测量
        const int target_rps = 1000; // 固定速率
        std::atomic<bool> stop_test{false};
        auto test_start_time = std::chrono::steady_clock::now();

        // 工作线程函数
        auto worker_func = [&](int worker_id) {
            std::random_device rd;
            std::mt19937 gen(rd());

            // 每个线程独立的HTTP客户端
            auto client = std::make_unique<HttpClient>(config.server_host, config.server_port, config.keep_alive);

            // 计算每个线程应该发送的请求速率
            int requests_per_worker = target_rps / config.concurrent_connections;
            if (requests_per_worker < 1) requests_per_worker = 1;

            auto next_request_time = std::chrono::steady_clock::now();

            while (!stop_test) {
                auto now = std::chrono::steady_clock::now();
                if (now < next_request_time) {
                    std::this_thread::sleep_until(next_request_time);
                }

                // 发送请求并测量延迟
                auto request_start = std::chrono::steady_clock::now();

                auto request_result = client->SendRequest(config.request_method, config.request_path,
                                                         config.request_body);

                auto request_end = std::chrono::steady_clock::now();
                double latency_ms = std::chrono::duration_cast<std::chrono::microseconds>(
                    request_end - request_start).count() / 1000.0;

                total_requests++;

                if (request_result.success) {
                    successful_requests++;

                    std::lock_guard<std::mutex> lock(latencies_mutex);
                    all_latencies.push_back(latency_ms);

                    // 如果收集时间序列数据
                    if (config.collect_time_series) {
                        BenchmarkResult::TimeSeriesPoint point;
                        point.timestamp = std::chrono::steady_clock::now();
                        point.value = latency_ms;
                        result.time_series.push_back(point);
                    }
                } else {
                    failed_requests++;
                }

                // 如果连接关闭（非keep-alive），创建新连接
                if (!request_result.success && !config.keep_alive) {
                    client = std::make_unique<HttpClient>(config.server_host, config.server_port, config.keep_alive);
                }

                // 安排下一个请求时间
                next_request_time += std::chrono::milliseconds(1000 / requests_per_worker);
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

        // 等待工作线程
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

        // 计算延迟统计
        if (!all_latencies.empty()) {
            std::sort(all_latencies.begin(), all_latencies.end());

            double qps = (test_duration > 0) ? successful_requests / test_duration : 0;
            double error_rate = (total_requests > 0) ?
                static_cast<double>(failed_requests) / total_requests : 0;

            // 计算各种百分位数
            std::vector<double> percentiles = {50.0, 75.0, 90.0, 95.0, 99.0, 99.9, 99.99};

            result.success = true;
            result.duration_seconds = test_duration;
            result.end_time = std::chrono::system_clock::now();

            // 添加基本指标
            result.metrics.push_back({"qps", qps, "requests/second", "每秒查询数"});
            result.metrics.push_back({"total_requests", static_cast<double>(total_requests), "requests", "总请求数"});
            result.metrics.push_back({"successful_requests", static_cast<double>(successful_requests), "requests", "成功请求数"});
            result.metrics.push_back({"failed_requests", static_cast<double>(failed_requests), "requests", "失败请求数"});
            result.metrics.push_back({"error_rate", error_rate * 100, "%", "错误率"});

            // 添加延迟统计
            double min_latency = Statistics::CalculateMin(all_latencies);
            double max_latency = Statistics::CalculateMax(all_latencies);
            double mean_latency = Statistics::CalculateMean(all_latencies);
            double median_latency = Statistics::CalculateMedian(all_latencies);
            double stddev_latency = Statistics::CalculateStdDev(all_latencies);

            result.metrics.push_back({"min_latency", min_latency, "ms", "最小延迟"});
            result.metrics.push_back({"max_latency", max_latency, "ms", "最大延迟"});
            result.metrics.push_back({"mean_latency", mean_latency, "ms", "平均延迟"});
            result.metrics.push_back({"median_latency", median_latency, "ms", "中位数延迟"});
            result.metrics.push_back({"stddev_latency", stddev_latency, "ms", "延迟标准差"});

            // 添加百分位数延迟
            for (double percentile : percentiles) {
                double value = Statistics::CalculatePercentile(all_latencies, percentile);
                std::string name = "p" + std::to_string(static_cast<int>(percentile));
                if (percentile == 99.9) name = "p99_9";
                if (percentile == 99.99) name = "p99_99";

                result.metrics.push_back({name + "_latency", value, "ms",
                                         std::to_string(static_cast<int>(percentile)) + "%分位延迟"});
            }

            // 计算延迟分布直方图（简化版）
            if (all_latencies.size() >= 10) {
                int num_buckets = 10;
                double bucket_size = (max_latency - min_latency) / num_buckets;
                if (bucket_size > 0) {
                    std::vector<int> histogram(num_buckets, 0);
                    for (double latency : all_latencies) {
                        int bucket = static_cast<int>((latency - min_latency) / bucket_size);
                        if (bucket >= num_buckets) bucket = num_buckets - 1;
                        histogram[bucket]++;
                    }

                    for (int i = 0; i < num_buckets; ++i) {
                        double bucket_start = min_latency + i * bucket_size;
                        double bucket_end = min_latency + (i + 1) * bucket_size;
                        double percentage = static_cast<double>(histogram[i]) / all_latencies.size() * 100;

                        std::string name = "latency_bucket_" + std::to_string(i);
                        std::string desc = "延迟在[" + std::to_string(bucket_start) + "-" +
                                          std::to_string(bucket_end) + "]ms的比例";
                        result.metrics.push_back({name, percentage, "%", desc});
                    }
                }
            }

            LOG_INFO("延迟基准测试完成: P99延迟=%.2fms, QPS=%.2f",
                    Statistics::CalculatePercentile(all_latencies, 99.0), qps);

        } else {
            result.success = false;
            result.error_message = "没有收集到有效的延迟数据";
        }

        CleanUp(config);
        return result;
    }

    void WarmUp(const BenchmarkConfig& config) override {
        LOG_INFO("延迟基准测试: 预热阶段开始");

        // 发送一些请求让服务器预热
        HttpClient client(config.server_host, config.server_port, config.keep_alive);
        for (int i = 0; i < 50; ++i) {
            client.SendRequest(config.request_method, config.request_path, config.request_body);
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        LOG_INFO("延迟基准测试: 预热阶段完成");
    }

    void CleanUp(const BenchmarkConfig& /*config*/) override {
        LOG_INFO("延迟基准测试: 清理完成");
    }

private:
    // 复用QPS测试中的HttpClient类
    class HttpClient {
    public:
        struct RequestResult {
            bool success;
            int64_t response_size;
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

            if (fd_ < 0 && !Connect()) {
                result.error_message = "连接失败";
                return result;
            }

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

            ssize_t sent = send(fd_, request.c_str(), request.size(), 0);
            if (sent < 0) {
                result.error_message = "发送失败: " + std::string(strerror(errno));
                CloseConnection();
                return result;
            }

            // 接收响应（简化版，只接收头部）
            char buffer[4096];
            std::string response;

            struct timeval tv;
            tv.tv_sec = 2;  // 较短的超时，因为主要关注延迟
            tv.tv_usec = 0;
            setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

            ssize_t received = recv(fd_, buffer, sizeof(buffer), 0);
            if (received < 0) {
                result.error_message = "接收失败: " + std::string(strerror(errno));
                CloseConnection();
                return result;
            } else if (received == 0) {
                if (!keep_alive_) {
                    CloseConnection();
                }
            } else {
                response.append(buffer, received);
                result.response_size = response.size();
            }

            result.success = true;

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

            struct timeval tv;
            tv.tv_sec = 2;
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
};

// 工厂函数
std::unique_ptr<Benchmark> CreateLatencyBenchmark() {
    return std::make_unique<LatencyBenchmark>();
}

} // namespace benchmark
} // namespace tinywebserver