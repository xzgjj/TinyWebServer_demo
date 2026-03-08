/**
 * @file benchmark_step_stress.cpp
 * @brief 阶梯式压力测试实现
 *
 * 逐步增加并发连接数，观察服务器在不同负载下的性能表现。
 * 测试阶段：10, 50, 100, 200, 500 并发连接（可配置）
 * 每个阶段运行固定时间，收集QPS、延迟、错误率等指标。
 */

#include "../../include/benchmark.h"
#include "../../include/server.h"
#include "../../include/Logger.h"
#include "../../include/http_response.h"
#include "../../include/http_request.h"
#include "../../include/request_validator.h"
#include "../../include/connection.h"
#include "../../include/plugin/plugin_manager.h"

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
#include <sstream>
#include <iomanip>
#include <numeric>
#include <algorithm>

namespace tinywebserver {
namespace benchmark {

// 复用QPS基准测试中的HttpClient类
// 这里直接包含或复制，为简单起见我们复制基本结构
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
            LOG_ERROR("HttpClient::SendRequest: 连接失败到 %s:%d", host_.c_str(), port_);
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
        ssize_t received;

        // 简单实现：接收直到连接关闭或超时
        // 实际实现应该解析Content-Length或Transfer-Encoding
        struct timeval tv;
        tv.tv_sec = 5;  // 5秒超时
        tv.tv_usec = 0;
        setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        while ((received = recv(fd_, buffer, sizeof(buffer), 0)) > 0) {
            response.append(buffer, received);
        }

        auto end_time = std::chrono::steady_clock::now();
        result.latency_ms = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time).count();
        result.response_size = response.size();

        // 简单检查响应是否有效（包含HTTP状态码）
        if (response.find("HTTP/1.1") != std::string::npos ||
            response.find("HTTP/1.0") != std::string::npos) {
            result.success = true;
            // 提取状态码
            size_t space_pos = response.find(' ');
            if (space_pos != std::string::npos) {
                size_t code_start = space_pos + 1;
                size_t code_end = response.find(' ', code_start);
                if (code_end != std::string::npos) {
                    result.status_code = std::stoi(response.substr(code_start, code_end - code_start));
                }
            }
        } else {
            result.error_message = "无效的HTTP响应";
            CloseConnection();
        }

        // 如果不保持连接，关闭socket
        if (!keep_alive_) {
            CloseConnection();
        }

        return result;
    }

private:
    bool Connect() {
        fd_ = socket(AF_INET, SOCK_STREAM, 0);
        if (fd_ < 0) {
            LOG_ERROR("HttpClient::Connect: socket创建失败: %s", strerror(errno));
            return false;
        }

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port_);
        if (inet_pton(AF_INET, host_.c_str(), &addr.sin_addr) <= 0) {
            LOG_ERROR("HttpClient::Connect: 地址转换失败: %s", host_.c_str());
            close(fd_);
            fd_ = -1;
            return false;
        }

        // 设置超时
        struct timeval tv;
        tv.tv_sec = 5;
        tv.tv_usec = 0;
        setsockopt(fd_, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
        setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        LOG_INFO("HttpClient: 正在连接到 %s:%d", host_.c_str(), port_);
        if (connect(fd_, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
            LOG_ERROR("HttpClient: 连接失败: %s (errno=%d)", strerror(errno), errno);
            close(fd_);
            fd_ = -1;
            return false;
        }

        LOG_INFO("HttpClient: 连接成功到 %s:%d", host_.c_str(), port_);
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
 * @brief 阶梯式压力测试实现
 */
class StepStressBenchmark : public Benchmark {
public:
    std::string GetName() const override {
        return "step_stress_benchmark";
    }

    std::string GetDescription() const override {
        return "阶梯式压力测试：逐步增加并发连接数，观察服务器性能变化";
    }

    // 最小测试验证：启动服务器，发送单个请求，验证基本功能
    bool RunMinimalTest(const BenchmarkConfig& config) {
        LOG_INFO("阶梯式压力测试: 开始运行最小测试验证");

        // 创建测试目录和文件
        system("mkdir -p public");
        system("echo '<h1>TinyWebServer Step Stress Test</h1>' > public/index.html");

        // 启动服务器
        std::unique_ptr<Server> server;
        try {
            server = std::make_unique<Server>(config.server_host, config.server_port, PluginManager::GetInstance());
            // 设置消息处理回调
            server->SetOnMessage([](std::shared_ptr<Connection> conn, const std::string& data) {
                auto parser = conn->GetHttpParser();
                auto& buffer = conn->GetInputBuffer();

                size_t header_end = buffer.find("\r\n\r\n");
                if (header_end != std::string::npos) {
                    if (parser->Parse(buffer)) {
                        // 生成简单响应
                        HttpResponse response;
                        response.Init("./public", parser->GetPath(), false, 200, parser.get());
                        response.MakeResponse();

                        // 发送响应
                        conn->Send(response.GetHeaderString());
                        if (response.HasFileBody()) {
                            conn->Send(response.GetFileBody());
                        } else {
                            conn->Send(response.GetBodyString());
                        }
                    }
                }
            });

            server->Start();
        } catch (const std::exception& e) {
            LOG_ERROR("阶梯式压力测试: 服务器启动失败: %s", e.what());
            return false;
        }

        // 等待服务器启动
        std::this_thread::sleep_for(std::chrono::milliseconds(500));

        // 发送测试请求
        HttpClient client(config.server_host, config.server_port, false);
        auto result = client.SendRequest("GET", "/index.html");

        // 停止服务器
        if (server) {
            server->Stop();
        }

        if (!result.success) {
            LOG_ERROR("阶梯式压力测试: 最小测试失败: %s", result.error_message.c_str());
            return false;
        }

        LOG_INFO("阶梯式压力测试: 最小测试通过，状态码: %d", result.status_code);
        return true;
    }

    BenchmarkResult Run(const BenchmarkConfig& config) override {
        BenchmarkResult result;
        result.name = GetName();
        result.start_time = std::chrono::system_clock::now();

        // 首先运行最小测试验证基本功能
        LOG_INFO("阶梯式压力测试: 开始运行最小测试验证");
        if (!RunMinimalTest(config)) {
            result.success = false;
            result.error_message = "最小测试验证失败，服务器无法处理请求";
            result.end_time = std::chrono::system_clock::now();
            return result;
        }

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

        // 定义测试阶段（并发连接数）
        std::vector<int> stages;
        // 从配置中获取阶段设置，或使用默认值
        std::string stage_config = "10,50,100,200,500"; // 默认阶段
        for (const auto& param : config.custom_params) {
            if (param.first == "stages") {
                stage_config = param.second;
                break;
            }
        }

        // 解析阶段配置
        std::istringstream iss(stage_config);
        std::string stage_str;
        while (std::getline(iss, stage_str, ',')) {
            try {
                stages.push_back(std::stoi(stage_str));
            } catch (...) {
                LOG_WARN("无效的阶段配置: %s", stage_str.c_str());
            }
        }

        if (stages.empty()) {
            stages = {10, 50, 100, 200, 500}; // 默认值
        }

        // 解析阶段持续时间
        int stage_duration = 10; // 默认10秒
        for (const auto& param : config.custom_params) {
            if (param.first == "stage_duration") {
                try {
                    stage_duration = std::stoi(param.second);
                } catch (...) {
                    LOG_WARN("无效的阶段持续时间配置: %s", param.second.c_str());
                }
                break;
            }
        }

        LOG_INFO("阶梯式压力测试: 测试阶段: %zu个阶段", stages.size());
        LOG_INFO("阶梯式压力测试: 每个阶段持续时间: %d 秒", stage_duration);
        for (size_t i = 0; i < stages.size(); ++i) {
            LOG_INFO("  阶段 %zu: %d 并发连接", i + 1, stages[i]);
        }

        // 启动服务器（在独立线程中）
        std::unique_ptr<Server> server;
        std::thread server_thread;

        try {
            // 确保public目录存在，包含测试文件
            system("mkdir -p public");
            system("echo '<h1>TinyWebServer Step Stress Test</h1>' > public/index.html");
            LOG_INFO("创建测试文件完成");

            server = std::make_unique<Server>(config.server_host, config.server_port, PluginManager::GetInstance());
            // 设置消息处理回调
            server->SetOnMessage([](std::shared_ptr<Connection> conn, const std::string& data) {
                auto parser = conn->GetHttpParser();
                auto& buffer = conn->GetInputBuffer();

                size_t header_end = buffer.find("\r\n\r\n");
                if (header_end != std::string::npos) {
                    if (parser->Parse(buffer)) {
                        // 生成简单响应
                        HttpResponse response;
                        response.Init("./public", parser->GetPath(), false, 200, parser.get());
                        response.MakeResponse();

                        // 发送响应
                        conn->Send(response.GetHeaderString());
                        if (response.HasFileBody()) {
                            conn->Send(response.GetFileBody());
                        } else {
                            conn->Send(response.GetBodyString());
                        }
                    }
                }
            });

            server_thread = std::thread([&server]() {
                server->Start();
            });

            // 等待服务器启动
            std::this_thread::sleep_for(std::chrono::seconds(1));

        } catch (const std::exception& e) {
            result.success = false;
            result.error_message = std::string("服务器启动失败: ") + e.what();
            result.end_time = std::chrono::system_clock::now();
            return result;
        }

        // 执行阶梯测试
        std::vector<BenchmarkResult::Metric> stage_metrics;
        int stage_num = 1;

        for (int concurrent_connections : stages) {
            LOG_INFO("=========================================");
            LOG_INFO("开始测试阶段 %d: %d 并发连接", stage_num, concurrent_connections);
            LOG_INFO("=========================================");

            // 为当前阶段创建配置
            BenchmarkConfig stage_config = config;
            stage_config.concurrent_connections = concurrent_connections;
            stage_config.duration_seconds = stage_duration; // 每个阶段运行时间

            // 运行当前阶段的测试
            auto stage_result = RunStage(stage_config, stage_num);

            // 添加阶段指标
            for (const auto& metric : stage_result.metrics) {
                // 重命名指标以包含阶段信息
                BenchmarkResult::Metric stage_metric = metric;
                stage_metric.name = "stage_" + std::to_string(stage_num) + "_" + metric.name;
                stage_metrics.push_back(stage_metric);

                // 同时添加到主结果中
                result.metrics.push_back(stage_metric);
            }

            // 添加阶段摘要指标
            result.metrics.push_back({
                "stage_" + std::to_string(stage_num) + "_concurrent",
                static_cast<double>(concurrent_connections),
                "connections",
                "阶段" + std::to_string(stage_num) + "并发连接数"
            });

            stage_num++;
        }

        // 停止服务器
        if (server) {
            server->Stop();
            if (server_thread.joinable()) {
                server_thread.join();
            }
        }

        // 计算总体指标
        CalculateOverallMetrics(result, stages);

        result.success = true;
        result.end_time = std::chrono::system_clock::now();
        result.duration_seconds = std::chrono::duration_cast<std::chrono::seconds>(
            result.end_time - result.start_time).count();

        LOG_INFO("阶梯式压力测试: 完成，总持续时间: %.2f 秒", result.duration_seconds);
        return result;
    }

private:
    /**
     * @brief 运行单个测试阶段
     */
    BenchmarkResult RunStage(const BenchmarkConfig& config, int stage_num) {
        BenchmarkResult result;
        result.name = "stage_" + std::to_string(stage_num);

        auto start_time = std::chrono::steady_clock::now();

        std::atomic<int64_t> total_requests{0};
        std::atomic<int64_t> failed_requests{0};
        std::vector<double> latencies;
        std::mutex latencies_mutex;

        // 创建客户端线程
        std::vector<std::thread> threads;
        std::atomic<bool> stop_flag{false};

        // 每个线程处理一部分连接
        int threads_count = std::min(config.concurrent_connections, 50); // 最多50个线程
        int connections_per_thread = config.concurrent_connections / threads_count;
        int remaining_connections = config.concurrent_connections % threads_count;

        LOG_INFO("阶段 %d: 启动 %d 个线程，每个线程处理 %d 个连接",
                stage_num, threads_count, connections_per_thread);

        for (int i = 0; i < threads_count; ++i) {
            int connections = connections_per_thread;
            if (i == 0) {
                connections += remaining_connections; // 第一个线程处理剩余的连接
            }

            threads.emplace_back([&, i, connections]() {
                std::vector<std::unique_ptr<HttpClient>> clients;

                // 创建客户端连接
                for (int j = 0; j < connections; ++j) {
                    clients.push_back(std::make_unique<HttpClient>(
                        config.server_host, config.server_port, config.keep_alive));
                }

                // 运行测试
                auto stage_start = std::chrono::steady_clock::now();
                while (!stop_flag.load() &&
                       std::chrono::duration_cast<std::chrono::seconds>(
                           std::chrono::steady_clock::now() - stage_start).count() < config.duration_seconds) {

                    // 每个客户端发送一个请求
                    for (auto& client : clients) {
                        auto request_result = client->SendRequest(
                            config.request_method, config.request_path, config.request_body);

                        total_requests.fetch_add(1, std::memory_order_relaxed);

                        if (!request_result.success) {
                            failed_requests.fetch_add(1, std::memory_order_relaxed);
                        } else {
                            std::lock_guard<std::mutex> lock(latencies_mutex);
                            latencies.push_back(static_cast<double>(request_result.latency_ms));
                        }

                        // 短暂休眠以避免过度负载
                        std::this_thread::sleep_for(std::chrono::milliseconds(10));
                    }
                }
            });
        }

        // 等待测试阶段完成
        std::this_thread::sleep_for(std::chrono::seconds(static_cast<int>(config.duration_seconds)));
        stop_flag.store(true);

        // 等待所有线程完成
        for (auto& thread : threads) {
            if (thread.joinable()) {
                thread.join();
            }
        }

        auto end_time = std::chrono::steady_clock::now();
        double duration_seconds = std::chrono::duration_cast<std::chrono::milliseconds>(
            end_time - start_time).count() / 1000.0;

        // 计算指标
        if (duration_seconds > 0) {
            double qps = total_requests.load() / duration_seconds;
            double error_rate = (total_requests.load() > 0) ?
                (failed_requests.load() * 100.0 / total_requests.load()) : 0.0;

            // 计算延迟统计
            double avg_latency = 0.0;
            double p99_latency = 0.0;

            if (!latencies.empty()) {
                std::sort(latencies.begin(), latencies.end());
                avg_latency = std::accumulate(latencies.begin(), latencies.end(), 0.0) / latencies.size();

                size_t p99_index = static_cast<size_t>(latencies.size() * 0.99);
                if (p99_index >= latencies.size()) p99_index = latencies.size() - 1;
                p99_latency = latencies[p99_index];
            }

            // 添加指标到结果
            result.metrics.push_back({"qps", qps, "requests/second", "每秒查询数"});
            result.metrics.push_back({"total_requests", static_cast<double>(total_requests.load()), "requests", "总请求数"});
            result.metrics.push_back({"failed_requests", static_cast<double>(failed_requests.load()), "requests", "失败请求数"});
            result.metrics.push_back({"error_rate", error_rate, "%", "错误率"});
            result.metrics.push_back({"avg_latency", avg_latency, "ms", "平均延迟"});
            result.metrics.push_back({"p99_latency", p99_latency, "ms", "P99延迟"});
            result.metrics.push_back({"duration", duration_seconds, "seconds", "测试持续时间"});
        }

        result.success = true;
        return result;
    }

    /**
     * @brief 计算总体指标
     */
    void CalculateOverallMetrics(BenchmarkResult& result, const std::vector<int>& stages) {
        if (stages.empty()) return;

        // 提取各阶段的QPS值
        std::vector<double> stage_qps_values;
        for (size_t i = 0; i < stages.size(); ++i) {
            for (const auto& metric : result.metrics) {
                if (metric.name == "stage_" + std::to_string(i + 1) + "_qps") {
                    stage_qps_values.push_back(metric.value);
                    break;
                }
            }
        }

        if (stage_qps_values.empty()) return;

        // 计算QPS变化趋势
        double max_qps = *std::max_element(stage_qps_values.begin(), stage_qps_values.end());
        double min_qps = *std::min_element(stage_qps_values.begin(), stage_qps_values.end());

        // 添加总体指标
        result.metrics.push_back({"max_qps", max_qps, "requests/second", "最大QPS"});
        result.metrics.push_back({"min_qps", min_qps, "requests/second", "最小QPS"});
        result.metrics.push_back({"qps_range", max_qps - min_qps, "requests/second", "QPS范围"});

        // 计算稳定性指标（QPS变化率）
        if (stage_qps_values.size() > 1) {
            double qps_change_sum = 0.0;
            for (size_t i = 1; i < stage_qps_values.size(); ++i) {
                double change = std::abs(stage_qps_values[i] - stage_qps_values[i-1]) / stage_qps_values[i-1];
                qps_change_sum += change;
            }
            double avg_qps_change = qps_change_sum / (stage_qps_values.size() - 1);
            result.metrics.push_back({"avg_qps_change", avg_qps_change * 100, "%", "平均QPS变化率"});
        }

        // 添加阶段数量信息
        result.metrics.push_back({"num_stages", static_cast<double>(stages.size()), "stages", "测试阶段数量"});
    }
};

// 工厂函数
std::unique_ptr<Benchmark> CreateStepStressBenchmark() {
    return std::make_unique<StepStressBenchmark>();
}

} // namespace benchmark
} // namespace tinywebserver