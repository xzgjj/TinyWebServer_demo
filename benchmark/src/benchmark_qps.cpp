/**
 * @file benchmark_qps.cpp
 * @brief QPS (每秒查询数) 基准测试实现
 */

#include "../../include/benchmark.h"
#include "../../include/server.h"
#include "../../include/Logger.h"
#include "../../include/http_response.h"
#include "../../include/http_request.h"
#include "../../include/request_validator.h"
#include "../../include/connection.h"

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

        // 设置接收超时
        struct timeval tv;
        tv.tv_sec = 5;
        tv.tv_usec = 0;
        setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        LOG_INFO("HttpClient: 开始接收响应");
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
            LOG_ERROR("HttpClient: 创建socket失败: %s", strerror(errno));
            return false;
        }

        struct sockaddr_in addr;
        std::memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port_);

        if (inet_pton(AF_INET, host_.c_str(), &addr.sin_addr) <= 0) {
            LOG_ERROR("HttpClient: 地址转换失败: %s:%d", host_.c_str(), port_);
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

    // 最小测试验证：启动服务器，发送单个请求，验证基本功能
    // 定义在类内部（内联）

    BenchmarkResult Run(const BenchmarkConfig& config) override {
        BenchmarkResult result;
        result.name = GetName();
        result.start_time = std::chrono::system_clock::now();

        // 首先运行最小测试验证基本功能
        LOG_INFO("QPS基准测试: 开始运行最小测试验证");
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

        // 启动服务器（在独立线程中）
        std::unique_ptr<Server> server;
        std::thread server_thread;

        try {
            // 确保public目录存在，包含测试文件（在build目录中）
            system("mkdir -p public");
            system("echo '<h1>TinyWebServer Benchmark Test</h1>' > public/index.html");
            LOG_INFO("创建测试文件完成");

            server = std::make_unique<Server>(config.server_host, config.server_port, PluginManager::GetInstance());
            // 设置消息处理回调（必须设置，否则服务器无法处理请求）
            server->SetOnMessage([](std::shared_ptr<Connection> conn, const std::string& data) {
                LOG_INFO("基准测试服务器回调: 收到数据，连接fd=%d，数据大小=%zu",
                        conn->GetFd(), data.size());
                auto parser = conn->GetHttpParser();
                auto& buffer = conn->GetInputBuffer();

                // 简单解析HTTP请求
                size_t header_end = buffer.find("\r\n\r\n");
                LOG_INFO("基准测试服务器回调: 查找header_end，缓冲区大小=%zu，header_end=%s",
                        buffer.size(), (header_end != std::string::npos ? "找到" : "未找到"));

                if (header_end != std::string::npos) {
                    LOG_INFO("基准测试服务器回调: 尝试解析请求...");
                    if (parser->Parse(buffer)) {
                        LOG_INFO("基准测试服务器回调: 请求解析成功: %s %s %s",
                                parser->GetMethod().c_str(), parser->GetPath().c_str(), parser->GetVersion().c_str());

                        // 生成简单响应
                        HttpResponse response;
                        LOG_INFO("基准测试服务器回调: 初始化响应，路径=./public/index.html");
                        response.Init("./public", "/index.html", false, -1, parser.get());
                        response.MakeResponse();

                        LOG_INFO("基准测试服务器回调: 发送响应头部");
                        conn->Send(response.GetHeaderString());
                        if (response.HasFileBody()) {
                            LOG_INFO("基准测试服务器回调: 发送文件内容");
                            conn->Send(response.GetFileBody());
                        } else {
                            LOG_INFO("基准测试服务器回调: 发送字符串内容");
                            conn->Send(response.GetBodyString());
                        }

                        buffer.erase(0, header_end + 4);
                        parser->Reset();
                        LOG_INFO("基准测试服务器回调: 请求处理完成");
                    } else {
                        LOG_ERROR("基准测试服务器回调: 请求解析失败，关闭连接");
                        buffer.clear();
                        conn->Shutdown();
                    }
                } else {
                    LOG_INFO("基准测试服务器回调: 数据不足，等待更多数据");
                }
            });
            server_thread = std::thread([&server]() {
                server->Start();
                server->Run();
            });

            // 等待服务器启动
            std::this_thread::sleep_for(std::chrono::milliseconds(2000));
            LOG_INFO("QPS基准测试: 服务器已启动在 %s:%d",
                    config.server_host.c_str(), config.server_port);

            // 验证服务器是否真的在监听端口
            LOG_INFO("验证服务器端口连接...");
            int test_fd = socket(AF_INET, SOCK_STREAM, 0);
            if (test_fd >= 0) {
                struct sockaddr_in addr;
                std::memset(&addr, 0, sizeof(addr));
                addr.sin_family = AF_INET;
                addr.sin_port = htons(config.server_port);
                inet_pton(AF_INET, config.server_host.c_str(), &addr.sin_addr);

                struct timeval tv;
                tv.tv_sec = 2;
                tv.tv_usec = 0;
                setsockopt(test_fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

                if (connect(test_fd, (struct sockaddr*)&addr, sizeof(addr)) == 0) {
                    LOG_INFO("端口连接验证成功");
                    close(test_fd);
                } else {
                    LOG_ERROR("端口连接验证失败: %s (errno=%d)", strerror(errno), errno);
                    close(test_fd);
                    result.success = false;
                    result.error_message = "服务器端口无法连接，可能服务器未正确启动";
                    result.end_time = std::chrono::system_clock::now();
                    // 停止服务器线程
                    if (server) server->Stop();
                    if (server_thread.joinable()) server_thread.join();
                    return result;
                }
            } else {
                LOG_ERROR("创建测试socket失败: %s", strerror(errno));
            }

            // 测试请求：发送一个简单的HTTP请求并打印响应
            LOG_INFO("发送测试请求...");
            HttpClient test_client(config.server_host, config.server_port, false);
            auto test_result = test_client.SendRequest("GET", "/index.html", "");
            if (test_result.success) {
                LOG_INFO("测试请求成功: 状态码=%d, 延迟=%ldms, 响应大小=%ld",
                        test_result.status_code, test_result.latency_ms, test_result.response_size);
            } else {
                LOG_ERROR("测试请求失败: %s", test_result.error_message.c_str());
                result.success = false;
                result.error_message = "测试请求失败: " + test_result.error_message;
                result.end_time = std::chrono::system_clock::now();
                if (server) server->Stop();
                if (server_thread.joinable()) server_thread.join();
                return result;
            }
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
        double p95_latency = 0;
        double p99_latency = 0;
        double p999_latency = 0;
        double min_latency = 0;
        double max_latency = 0;
        double stddev_latency = 0;

        if (!latencies.empty()) {
            std::sort(latencies.begin(), latencies.end());

            double sum_latency = 0;
            for (double latency : latencies) {
                sum_latency += latency;
            }
            avg_latency = sum_latency / latencies.size();

            p50_latency = Statistics::CalculatePercentile(latencies, 50.0);
            p90_latency = Statistics::CalculatePercentile(latencies, 90.0);
            p95_latency = Statistics::CalculatePercentile(latencies, 95.0);
            p99_latency = Statistics::CalculatePercentile(latencies, 99.0);
            p999_latency = Statistics::CalculatePercentile(latencies, 99.9);
            min_latency = Statistics::CalculateMin(latencies);
            max_latency = Statistics::CalculateMax(latencies);
            stddev_latency = Statistics::CalculateStdDev(latencies);
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
        result.metrics.push_back({"p95_latency", p95_latency, "ms", "95%分位延迟"});
        result.metrics.push_back({"p99_latency", p99_latency, "ms", "99%分位延迟"});
        result.metrics.push_back({"p999_latency", p999_latency, "ms", "99.9%分位延迟"});
        result.metrics.push_back({"min_latency", min_latency, "ms", "最小延迟"});
        result.metrics.push_back({"max_latency", max_latency, "ms", "最大延迟"});
        result.metrics.push_back({"stddev_latency", stddev_latency, "ms", "延迟标准差"});
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
        std::atomic<int> warmup_success{0};
        std::atomic<int> warmup_failed{0};

        for (int i = 0; i < std::min(10, config.concurrent_connections); ++i) {
            warmup_threads.emplace_back([&config, i, &warmup_success, &warmup_failed]() {
                HttpClient client(config.server_host, config.server_port, config.keep_alive);
                for (int j = 0; j < warmup_requests / 10; ++j) {
                    auto result = client.SendRequest(config.request_method, config.request_path, config.request_body);
                    if (result.success) {
                        warmup_success++;
                        LOG_INFO("预热请求成功: 状态码=%d, 延迟=%ldms, 大小=%ld (线程%d, 请求%d)",
                                result.status_code, result.latency_ms, result.response_size, i, j);
                    } else {
                        warmup_failed++;
                        LOG_WARN("预热请求失败: %s (线程%d, 请求%d)",
                                result.error_message.c_str(), i, j);
                    }
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
        LOG_INFO("QPS基准测试: 预热阶段完成, 成功: %d, 失败: %d",
                warmup_success.load(), warmup_failed.load());
    }

    inline bool RunMinimalTest(const BenchmarkConfig& config) {
        LOG_INFO("=== 开始最小测试验证 ===");
        // 使用备用端口8081，避免与可能存在的其他服务器冲突
        int test_port = 8081;
        LOG_INFO("服务器: %s:%d (使用端口 %d 避免冲突), 路径: %s",
                config.server_host.c_str(), config.server_port, test_port, config.request_path.c_str());

        // 确保public目录存在
        system("mkdir -p public");
        system("echo '<h1>TinyWebServer Benchmark Test</h1>' > public/index.html");
        LOG_INFO("测试文件准备完成");

        // 启动服务器
        std::unique_ptr<Server> server;
        std::thread server_thread;
        bool test_success = false;

        try {
            server = std::make_unique<Server>(config.server_host, test_port, PluginManager::GetInstance());

            // 设置消息处理回调（使用与main.cpp相同的逻辑）
            server->SetOnMessage([this](std::shared_ptr<Connection> conn, const std::string& /*data*/) {
                LOG_INFO("最小测试: 收到请求，开始处理");
                auto parser = conn->GetHttpParser();
                auto& buffer = conn->GetInputBuffer();

                // 查找请求头结束标记
                size_t header_end = buffer.find("\r\n\r\n");
                if (header_end == std::string::npos) {
                    LOG_INFO("最小测试: 数据不足，等待更多数据");
                    return;
                }

                LOG_INFO("最小测试: 尝试解析请求，缓冲区大小=%zu", buffer.size());
                if (parser->Parse(buffer)) {
                    LOG_INFO("最小测试: 请求解析成功: %s %s",
                            parser->GetMethod().c_str(), parser->GetPath().c_str());

                    // 生成响应（使用与基准测试相同的逻辑）
                    HttpResponse response;
                    response.Init("./public", "/index.html", false, -1, parser.get());
                    response.MakeResponse();

                    conn->Send(response.GetHeaderString());
                    if (response.HasFileBody()) {
                        conn->Send(response.GetFileBody());
                    } else {
                        conn->Send(response.GetBodyString());
                    }

                    buffer.erase(0, header_end + 4);
                    parser->Reset();
                    LOG_INFO("最小测试: 响应已发送");
                } else {
                    LOG_ERROR("最小测试: 请求解析失败");
                    buffer.clear();
                    conn->Shutdown();
                }
            });

            server_thread = std::thread([&server]() {
                server->Start();
                server->Run();
            });

            // 等待服务器启动
            std::this_thread::sleep_for(std::chrono::milliseconds(2000));
            LOG_INFO("最小测试: 服务器已启动");

            // 发送测试请求
            HttpClient test_client(config.server_host, test_port, false);
            LOG_INFO("最小测试: 发送测试请求到 %s:%d%s",
                    config.server_host.c_str(), test_port, config.request_path.c_str());

            auto test_result = test_client.SendRequest("GET", "/index.html", "");

            if (test_result.success) {
                LOG_INFO("最小测试: 请求成功! 状态码=%d, 延迟=%ldms, 响应大小=%ld",
                        test_result.status_code, test_result.latency_ms, test_result.response_size);
                test_success = true;
            } else {
                LOG_ERROR("最小测试: 请求失败: %s", test_result.error_message.c_str());
            }

            // 停止服务器
            if (server) {
                server->Stop();
            }
            if (server_thread.joinable()) {
                server_thread.join();
            }

        } catch (const std::exception& e) {
            LOG_ERROR("最小测试: 异常: %s", e.what());
            if (server) server->Stop();
            if (server_thread.joinable()) server_thread.join();
        }

        LOG_INFO("=== 最小测试验证 %s ===", test_success ? "成功" : "失败");
        return test_success;
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