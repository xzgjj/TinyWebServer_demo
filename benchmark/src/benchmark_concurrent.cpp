/**
 * @file benchmark_concurrent.cpp
 * @brief 并发连接能力基准测试实现
 */

#include "../../include/benchmark.h"
#include "../../include/server.h"
#include "../../include/Logger.h"

#include <atomic>
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/resource.h>
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
 * @brief 并发基准测试实现
 *
 * 测量服务器处理大量并发连接的能力：
 * 1. 最大并发连接数
 * 2. 连接建立速率
 * 3. 连接保持稳定性
 */
class ConcurrentBenchmark : public Benchmark {
public:
    std::string GetName() const override {
        return "concurrent_benchmark";
    }

    std::string GetDescription() const override {
        return "测量服务器并发连接处理能力";
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
            server = std::make_unique<Server>(config.server_host, config.server_port);
            server_thread = std::thread([&server]() {
                server->Run();
            });

            std::this_thread::sleep_for(std::chrono::milliseconds(2000));
            LOG_INFO("并发基准测试: 服务器已启动");
        } catch (const std::exception& e) {
            result.success = false;
            result.error_message = std::string("启动服务器失败: ") + e.what();
            result.end_time = std::chrono::system_clock::now();
            return result;
        }

        // 预热
        WarmUp(config);

        // 阶段1：测量最大并发连接数
        LOG_INFO("并发基准测试: 阶段1 - 测量最大并发连接数");
        auto max_connections_result = TestMaxConnections(config);

        // 阶段2：测量连接建立速率
        LOG_INFO("并发基准测试: 阶段2 - 测量连接建立速率");
        auto connection_rate_result = TestConnectionRate(config);

        // 阶段3：测量连接保持稳定性
        LOG_INFO("并发基准测试: 阶段3 - 测量连接保持稳定性");
        auto stability_result = TestConnectionStability(config);

        // 停止服务器
        if (server) {
            server->Stop();
        }
        if (server_thread.joinable()) {
            server_thread.join();
        }

        // 合并结果
        result.success = max_connections_result.success &&
                        connection_rate_result.success &&
                        stability_result.success;

        result.end_time = std::chrono::system_clock::now();
        result.duration_seconds = std::chrono::duration<double>(
            result.end_time - result.start_time).count();

        // 合并指标
        if (max_connections_result.success) {
            for (const auto& metric : max_connections_result.metrics) {
                result.metrics.push_back({"max_" + metric.name, metric.value,
                                         metric.unit, "最大并发-" + metric.description});
            }
        }

        if (connection_rate_result.success) {
            for (const auto& metric : connection_rate_result.metrics) {
                result.metrics.push_back({"rate_" + metric.name, metric.value,
                                         metric.unit, "连接速率-" + metric.description});
            }
        }

        if (stability_result.success) {
            for (const auto& metric : stability_result.metrics) {
                result.metrics.push_back({"stability_" + metric.name, metric.value,
                                         metric.unit, "稳定性-" + metric.description});
            }
        }

        // 计算总体评分
        if (result.metrics.size() >= 3) {
            double overall_score = CalculateOverallScore(result.metrics);
            result.metrics.push_back({"overall_score", overall_score, "points",
                                     "并发能力综合评分（0-100）"});
        }

        LOG_INFO("并发基准测试完成: 最大连接数=%d, 综合评分=%.1f",
                static_cast<int>(FindMetric(result.metrics, "max_successful_connections").value),
                FindMetric(result.metrics, "overall_score").value);

        CleanUp(config);
        return result;
    }

    void WarmUp(const BenchmarkConfig& config) override {
        LOG_INFO("并发基准测试: 预热阶段开始");

        // 建立少量连接确保服务器正常工作
        std::vector<std::thread> warmup_threads;
        const int warmup_connections = 10;

        for (int i = 0; i < warmup_connections; ++i) {
            warmup_threads.emplace_back([&config, i]() {
                int fd = socket(AF_INET, SOCK_STREAM, 0);
                if (fd < 0) return;

                struct sockaddr_in addr;
                std::memset(&addr, 0, sizeof(addr));
                addr.sin_family = AF_INET;
                addr.sin_port = htons(config.server_port);
                inet_pton(AF_INET, config.server_host.c_str(), &addr.sin_addr);

                if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) == 0) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                    const char* request = "GET / HTTP/1.1\r\nHost: localhost\r\n\r\n";
                    send(fd, request, strlen(request), 0);

                    char buffer[1024];
                    recv(fd, buffer, sizeof(buffer), 0);
                }

                close(fd);
            });
        }

        for (auto& thread : warmup_threads) {
            if (thread.joinable()) {
                thread.join();
            }
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        LOG_INFO("并发基准测试: 预热阶段完成");
    }

    void CleanUp(const BenchmarkConfig& /*config*/) override {
        LOG_INFO("并发基准测试: 清理完成");
    }

private:
    // 测试最大并发连接数
    BenchmarkResult TestMaxConnections(const BenchmarkConfig& config) {
        BenchmarkResult result;
        result.name = "max_connections_test";
        result.start_time = std::chrono::system_clock::now();

        const int max_test_connections = 10000; // 测试上限
        const int batch_size = 100; // 每批建立的连接数

        std::atomic<int> successful_connections{0};
        std::atomic<int> failed_connections{0};
        std::vector<int> open_fds;
        std::mutex fds_mutex;

        // 逐步增加连接数，直到达到上限或失败率过高
        int current_target = batch_size;
        bool reached_limit = false;

        auto test_start_time = std::chrono::steady_clock::now();

        while (current_target <= max_test_connections && !reached_limit) {
            LOG_INFO("测试 %d 个并发连接...", current_target);

            // 建立一批新连接
            std::vector<std::thread> connection_threads;
            std::atomic<int> batch_success{0};
            std::atomic<int> batch_fail{0};

            for (int i = 0; i < batch_size; ++i) {
                connection_threads.emplace_back([&, i]() {
                    int fd = socket(AF_INET, SOCK_STREAM, 0);
                    if (fd < 0) {
                        batch_fail++;
                        return;
                    }

                    // 设置非阻塞以控制超时
                    int flags = fcntl(fd, F_GETFL, 0);
                    fcntl(fd, F_SETFL, flags | O_NONBLOCK);

                    struct sockaddr_in addr;
                    std::memset(&addr, 0, sizeof(addr));
                    addr.sin_family = AF_INET;
                    addr.sin_port = htons(config.server_port);
                    inet_pton(AF_INET, config.server_host.c_str(), &addr.sin_addr);

                    int connect_result = connect(fd, (struct sockaddr*)&addr, sizeof(addr));
                    if (connect_result == 0) {
                        // 立即连接成功
                        batch_success++;
                        std::lock_guard<std::mutex> lock(fds_mutex);
                        open_fds.push_back(fd);
                    } else if (errno == EINPROGRESS) {
                        // 连接进行中，使用poll等待
                        struct pollfd pfd;
                        pfd.fd = fd;
                        pfd.events = POLLOUT;

                        int poll_result = poll(&pfd, 1, 1000); // 1秒超时
                        if (poll_result > 0 && (pfd.revents & POLLOUT)) {
                            // 检查连接是否真正成功
                            int error = 0;
                            socklen_t len = sizeof(error);
                            getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &len);

                            if (error == 0) {
                                batch_success++;
                                std::lock_guard<std::mutex> lock(fds_mutex);
                                open_fds.push_back(fd);
                            } else {
                                batch_fail++;
                                close(fd);
                            }
                        } else {
                            batch_fail++;
                            close(fd);
                        }
                    } else {
                        batch_fail++;
                        close(fd);
                    }
                });
            }

            // 等待所有连接尝试完成
            for (auto& thread : connection_threads) {
                if (thread.joinable()) {
                    thread.join();
                }
            }

            successful_connections += batch_success;
            failed_connections += batch_fail;

            // 计算本批成功率
            double batch_success_rate = (batch_success + batch_fail) > 0 ?
                static_cast<double>(batch_success) / (batch_success + batch_fail) : 0;

            LOG_INFO("批次结果: 成功 %d, 失败 %d, 成功率 %.1f%%",
                    batch_success.load(), batch_fail.load(), batch_success_rate * 100);

            // 如果成功率低于90%，认为达到极限
            if (batch_success_rate < 0.9) {
                reached_limit = true;
                LOG_INFO("达到连接极限 (成功率 < 90%%)");
            } else {
                // 保持当前连接，增加下一批目标
                current_target += batch_size;
                std::this_thread::sleep_for(std::chrono::milliseconds(500)); // 让服务器稳定
            }
        }

        auto test_end_time = std::chrono::steady_clock::now();
        double test_duration = std::chrono::duration<double>(test_end_time - test_start_time).count();

        // 关闭所有打开的文件描述符
        {
            std::lock_guard<std::mutex> lock(fds_mutex);
            for (int fd : open_fds) {
                close(fd);
            }
            open_fds.clear();
        }

        // 计算结果
        result.success = true;
        result.end_time = std::chrono::system_clock::now();
        result.duration_seconds = test_duration;

        int total_attempts = successful_connections + failed_connections;
        double success_rate = total_attempts > 0 ?
            static_cast<double>(successful_connections) / total_attempts : 0;
        double connections_per_second = test_duration > 0 ?
            total_attempts / test_duration : 0;

        result.metrics.push_back({"max_successful_connections",
                                 static_cast<double>(successful_connections),
                                 "connections", "最大成功并发连接数"});
        result.metrics.push_back({"max_connection_success_rate", success_rate * 100,
                                 "%", "最大连接测试成功率"});
        result.metrics.push_back({"connection_attempt_rate", connections_per_second,
                                 "connections/second", "连接尝试速率"});
        result.metrics.push_back({"total_connection_attempts", static_cast<double>(total_attempts),
                                 "attempts", "总连接尝试数"});

        // 记录达到的连接数级别
        std::string level = "low";
        if (successful_connections >= 5000) level = "excellent";
        else if (successful_connections >= 1000) level = "good";
        else if (successful_connections >= 500) level = "moderate";

        result.metrics.push_back({"connection_capacity_level", 0, "category", "连接容量等级 (" + level + ")"});

        return result;
    }

    // 测试连接建立速率
    BenchmarkResult TestConnectionRate(const BenchmarkConfig& config) {
        BenchmarkResult result;
        result.name = "connection_rate_test";
        result.start_time = std::chrono::system_clock::now();

        const int test_duration_seconds = 10;
        const int max_connections_per_second = 1000;

        std::atomic<int64_t> total_connections{0};
        std::atomic<int64_t> successful_connections{0};
        std::atomic<bool> stop_test{false};

        std::vector<std::thread> worker_threads;
        const int num_workers = 10;

        auto test_start_time = std::chrono::steady_clock::now();

        // 工作线程函数：持续建立连接
        auto worker_func = [&](int worker_id) {
            std::random_device rd;
            std::mt19937 gen(rd());
            std::uniform_int_distribution<> dist(1, 100);

            while (!stop_test) {
                int fd = socket(AF_INET, SOCK_STREAM, 0);
                if (fd < 0) {
                    continue;
                }

                // 设置连接超时
                struct timeval tv;
                tv.tv_sec = 1;
                tv.tv_usec = 0;
                setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

                struct sockaddr_in addr;
                std::memset(&addr, 0, sizeof(addr));
                addr.sin_family = AF_INET;
                addr.sin_port = htons(config.server_port);
                inet_pton(AF_INET, config.server_host.c_str(), &addr.sin_addr);

                total_connections++;

                if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) == 0) {
                    successful_connections++;
                    // 立即关闭连接（测试连接建立速率，不是保持）
                    close(fd);
                } else {
                    close(fd);
                }

                // 控制速率：每个工作线程大约每秒建立100个连接
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        };

        // 启动工作线程
        for (int i = 0; i < num_workers; ++i) {
            worker_threads.emplace_back(worker_func, i);
        }

        // 运行指定时间
        std::this_thread::sleep_for(std::chrono::seconds(test_duration_seconds));

        // 停止测试
        stop_test = true;

        for (auto& thread : worker_threads) {
            if (thread.joinable()) {
                thread.join();
            }
        }

        auto test_end_time = std::chrono::steady_clock::now();
        double test_duration = std::chrono::duration<double>(test_end_time - test_start_time).count();

        // 计算结果
        result.success = true;
        result.end_time = std::chrono::system_clock::now();
        result.duration_seconds = test_duration;

        double connections_per_second = test_duration > 0 ?
            total_connections / test_duration : 0;
        double successful_connections_per_second = test_duration > 0 ?
            successful_connections / test_duration : 0;
        double success_rate = total_connections > 0 ?
            static_cast<double>(successful_connections) / total_connections : 0;

        result.metrics.push_back({"connection_rate_total", connections_per_second,
                                 "connections/second", "总连接尝试速率"});
        result.metrics.push_back({"connection_rate_successful", successful_connections_per_second,
                                 "connections/second", "成功连接建立速率"});
        result.metrics.push_back({"connection_rate_success_rate", success_rate * 100,
                                 "%", "连接建立成功率"});
        result.metrics.push_back({"total_connection_attempts_during_test",
                                 static_cast<double>(total_connections),
                                 "attempts", "测试期间总连接尝试数"});

        return result;
    }

    // 测试连接保持稳定性
    BenchmarkResult TestConnectionStability(const BenchmarkConfig& config) {
        BenchmarkResult result;
        result.name = "connection_stability_test";
        result.start_time = std::chrono::system_clock::now();

        const int test_connections = 100;
        const int test_duration_seconds = 30;

        std::vector<int> fds;
        std::vector<std::thread> connection_threads;
        std::atomic<int> active_connections{0};
        std::atomic<int> lost_connections{0};
        std::atomic<bool> stop_test{false};

        // 建立初始连接
        LOG_INFO("建立 %d 个测试连接...", test_connections);

        for (int i = 0; i < test_connections; ++i) {
            connection_threads.emplace_back([&, i]() {
                int fd = socket(AF_INET, SOCK_STREAM, 0);
                if (fd < 0) return;

                struct sockaddr_in addr;
                std::memset(&addr, 0, sizeof(addr));
                addr.sin_family = AF_INET;
                addr.sin_port = htons(config.server_port);
                inet_pton(AF_INET, config.server_host.c_str(), &addr.sin_addr);

                if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) == 0) {
                    active_connections++;

                    // 保持连接活动：定期发送心跳
                    auto last_activity = std::chrono::steady_clock::now();

                    while (!stop_test) {
                        auto now = std::chrono::steady_clock::now();
                        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - last_activity);

                        if (elapsed.count() >= 5) { // 每5秒发送一次心跳
                            const char* heartbeat = "GET / HTTP/1.1\r\nHost: localhost\r\n\r\n";
                            ssize_t sent = send(fd, heartbeat, strlen(heartbeat), MSG_NOSIGNAL);

                            if (sent < 0) {
                                // 连接丢失
                                lost_connections++;
                                active_connections--;
                                break;
                            }

                            // 接收响应（忽略内容）
                            char buffer[1024];
                            struct timeval tv;
                            tv.tv_sec = 1;
                            tv.tv_usec = 0;
                            setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

                            recv(fd, buffer, sizeof(buffer), 0);
                            last_activity = now;
                        }

                        std::this_thread::sleep_for(std::chrono::milliseconds(100));
                    }
                }

                close(fd);
            });
        }

        // 等待所有连接建立
        std::this_thread::sleep_for(std::chrono::seconds(2));

        int initial_active = active_connections;
        LOG_INFO("初始活动连接: %d/%d", initial_active, test_connections);

        auto test_start_time = std::chrono::steady_clock::now();

        // 监控连接稳定性
        std::vector<int> active_history;
        std::thread monitor_thread([&]() {
            while (!stop_test) {
                active_history.push_back(active_connections);
                std::this_thread::sleep_for(std::chrono::seconds(1));
            }
        });

        // 运行稳定性测试
        std::this_thread::sleep_for(std::chrono::seconds(test_duration_seconds));

        // 停止测试
        stop_test = true;

        // 等待所有线程结束
        for (auto& thread : connection_threads) {
            if (thread.joinable()) {
                thread.join();
            }
        }

        if (monitor_thread.joinable()) {
            monitor_thread.join();
        }

        auto test_end_time = std::chrono::steady_clock::now();
        double test_duration = std::chrono::duration<double>(test_end_time - test_start_time).count();

        // 计算结果
        result.success = true;
        result.end_time = std::chrono::system_clock::now();
        result.duration_seconds = test_duration;

        int final_active = active_connections;
        int total_lost = lost_connections;
        double lost_percentage = initial_active > 0 ?
            static_cast<double>(total_lost) / initial_active * 100 : 0;

        // 计算稳定性指标
        double avg_active = 0;
        if (!active_history.empty()) {
            for (int count : active_history) {
                avg_active += count;
            }
            avg_active /= active_history.size();
        }

        double stability_score = 100.0 - std::min(lost_percentage * 2, 100.0); // 简单评分

        result.metrics.push_back({"initial_active_connections", static_cast<double>(initial_active),
                                 "connections", "初始活动连接数"});
        result.metrics.push_back({"final_active_connections", static_cast<double>(final_active),
                                 "connections", "最终活动连接数"});
        result.metrics.push_back({"lost_connections", static_cast<double>(total_lost),
                                 "connections", "丢失的连接数"});
        result.metrics.push_back({"connection_loss_rate", lost_percentage,
                                 "%", "连接丢失率"});
        result.metrics.push_back({"average_active_connections", avg_active,
                                 "connections", "平均活动连接数"});
        result.metrics.push_back({"connection_stability_score", stability_score,
                                 "points", "连接稳定性评分（0-100）"});

        return result;
    }

    // 辅助函数：查找指标
    BenchmarkResult::Metric FindMetric(const std::vector<BenchmarkResult::Metric>& metrics,
                                      const std::string& name) {
        for (const auto& metric : metrics) {
            if (metric.name == name) {
                return metric;
            }
        }
        return BenchmarkResult::Metric{name, 0.0, "", ""};
    }

    // 计算总体评分
    double CalculateOverallScore(const std::vector<BenchmarkResult::Metric>& metrics) {
        double max_connections_score = 0;
        double connection_rate_score = 0;
        double stability_score = 0;

        // 提取关键指标
        for (const auto& metric : metrics) {
            if (metric.name == "max_successful_connections") {
                // 根据最大连接数评分
                double connections = metric.value;
                if (connections >= 5000) max_connections_score = 100;
                else if (connections >= 1000) max_connections_score = 80;
                else if (connections >= 500) max_connections_score = 60;
                else if (connections >= 100) max_connections_score = 40;
                else max_connections_score = 20;
            }
            else if (metric.name == "connection_rate_successful") {
                // 根据连接建立速率评分
                double rate = metric.value;
                if (rate >= 500) connection_rate_score = 100;
                else if (rate >= 200) connection_rate_score = 80;
                else if (rate >= 100) connection_rate_score = 60;
                else if (rate >= 50) connection_rate_score = 40;
                else connection_rate_score = 20;
            }
            else if (metric.name == "connection_stability_score") {
                stability_score = metric.value;
            }
        }

        // 加权平均
        return (max_connections_score * 0.4 + connection_rate_score * 0.3 + stability_score * 0.3);
    }
};

// 工厂函数
std::unique_ptr<Benchmark> CreateConcurrentBenchmark() {
    return std::make_unique<ConcurrentBenchmark>();
}

} // namespace benchmark
} // namespace tinywebserver