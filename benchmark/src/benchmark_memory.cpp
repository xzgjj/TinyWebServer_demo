/**
 * @file benchmark_memory.cpp
 * @brief 内存使用基准测试实现
 */

#include "../../include/benchmark.h"
#include "../../include/server.h"
#include "../../include/Logger.h"

#include <atomic>
#include <chrono>
#include <cstring>
#include <dirent.h>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/resource.h>
#include <thread>
#include <unistd.h>
#include <vector>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <arpa/inet.h>

namespace tinywebserver {
namespace benchmark {

/**
 * @brief 系统资源监控器实现
 */
class SystemResourceMonitor::Impl {
public:
    Impl() : running_(false), peak_rss_kb_(0), total_cpu_time_ms_(0) {
        // 获取初始资源使用情况
        baseline_metrics_ = GetCurrentMetricsInternal();
    }

    ~Impl() {
        Stop();
    }

    void Start() {
        if (running_) return;

        running_ = true;
        monitor_thread_ = std::thread([this]() { MonitorLoop(); });
    }

    void Stop() {
        if (!running_) return;

        running_ = false;
        if (monitor_thread_.joinable()) {
            monitor_thread_.join();
        }
    }

    ResourceMetrics GetCurrentMetrics() const {
        std::lock_guard<std::mutex> lock(metrics_mutex_);
        return GetCurrentMetricsInternal();
    }

    std::vector<ResourceMetrics> GetTimeSeries() const {
        std::lock_guard<std::mutex> lock(time_series_mutex_);
        return time_series_;
    }

    int64_t GetPeakMemoryKb() const {
        std::lock_guard<std::mutex> lock(metrics_mutex_);
        return peak_rss_kb_;
    }

    double GetAverageCpuUsage() const {
        std::lock_guard<std::mutex> lock(time_series_mutex_);
        if (time_series_.empty()) return 0.0;

        double total_cpu_percent = 0.0;
        int count = 0;

        for (size_t i = 1; i < time_series_.size(); ++i) {
            const auto& prev = time_series_[i-1];
            const auto& curr = time_series_[i];

            auto time_diff = std::chrono::duration_cast<std::chrono::milliseconds>(
                curr.timestamp - prev.timestamp).count();
            if (time_diff > 0) {
                double cpu_diff = curr.cpu_time_ms - prev.cpu_time_ms;
                double cpu_percent = (cpu_diff / time_diff) * 100.0;
                total_cpu_percent += cpu_percent;
                count++;
            }
        }

        return count > 0 ? total_cpu_percent / count : 0.0;
    }

private:
    void MonitorLoop() {
        constexpr int kMonitorIntervalMs = 100; // 100ms采样间隔

        while (running_) {
            auto metrics = GetCurrentMetricsInternal();

            {
                std::lock_guard<std::mutex> lock(metrics_mutex_);
                if (metrics.rss_kb > peak_rss_kb_) {
                    peak_rss_kb_ = metrics.rss_kb;
                }
            }

            {
                std::lock_guard<std::mutex> lock(time_series_mutex_);
                time_series_.push_back(metrics);
                // 限制时间序列大小，避免内存爆炸
                if (time_series_.size() > 10000) {
                    time_series_.erase(time_series_.begin(), time_series_.begin() + 5000);
                }
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(kMonitorIntervalMs));
        }
    }

    ResourceMetrics GetCurrentMetricsInternal() const {
        ResourceMetrics metrics;
        metrics.timestamp = std::chrono::steady_clock::now();

        // 读取/proc/self/statm获取内存信息
        std::ifstream statm_file("/proc/self/statm");
        if (statm_file.is_open()) {
            long pages;
            statm_file >> pages; // 虚拟内存大小（页数）
            metrics.vm_size_kb = pages * getpagesize() / 1024;

            statm_file >> pages; // 驻留集大小（页数）
            metrics.rss_kb = pages * getpagesize() / 1024;
            statm_file.close();
        } else {
            // 回退方案：使用getrusage
            struct rusage usage;
            if (getrusage(RUSAGE_SELF, &usage) == 0) {
                metrics.rss_kb = usage.ru_maxrss; // 单位是KB
                metrics.vm_size_kb = 0; // getrusage不提供虚拟内存大小
            }
        }

        // 读取/proc/self/stat获取CPU时间
        std::ifstream stat_file("/proc/self/stat");
        if (stat_file.is_open()) {
            std::string line;
            std::getline(stat_file, line);
            std::istringstream iss(line);

            // 跳过前13个字段
            std::string token;
            for (int i = 0; i < 13; ++i) {
                iss >> token;
            }

            // 第14-15个字段是用户态和内核态CPU时间（时钟滴答数）
            long utime, stime;
            iss >> utime >> stime;

            long clock_ticks_per_second = sysconf(_SC_CLK_TCK);
            if (clock_ticks_per_second > 0) {
                metrics.cpu_time_ms = (utime + stime) * 1000 / clock_ticks_per_second;
            }
            stat_file.close();
        }

        // 获取文件描述符数量
        metrics.fd_count = CountOpenFileDescriptors();

        // 获取上下文切换和缺页次数（从/proc/self/status）
        std::ifstream status_file("/proc/self/status");
        if (status_file.is_open()) {
            std::string line;
            while (std::getline(status_file, line)) {
                if (line.find("voluntary_ctxt_switches:") == 0) {
                    std::istringstream iss(line.substr(25));
                    long voluntary, nonvoluntary;
                    iss >> voluntary;
                    if (line.find("nonvoluntary_ctxt_switches:") == 0) {
                        std::getline(status_file, line);
                        std::istringstream iss2(line.substr(29));
                        iss2 >> nonvoluntary;
                        metrics.context_switches = voluntary + nonvoluntary;
                    }
                } else if (line.find("VmPeak:") == 0) {
                    std::istringstream iss(line.substr(7));
                    std::string value;
                    iss >> value;
                    // 移除"kB"后缀并转换
                    if (!value.empty() && value.find("kB") != std::string::npos) {
                        value = value.substr(0, value.find("kB"));
                    }
                    try {
                        metrics.vm_size_kb = std::stol(value);
                    } catch (...) {
                        // 忽略转换错误
                    }
                } else if (line.find("VmRSS:") == 0) {
                    std::istringstream iss(line.substr(6));
                    std::string value;
                    iss >> value;
                    if (!value.empty() && value.find("kB") != std::string::npos) {
                        value = value.substr(0, value.find("kB"));
                    }
                    try {
                        metrics.rss_kb = std::stol(value);
                    } catch (...) {
                        // 忽略转换错误
                    }
                }
            }
            status_file.close();
        }

        return metrics;
    }

    int CountOpenFileDescriptors() const {
        int count = 0;
        DIR* dir = opendir("/proc/self/fd");
        if (dir) {
            struct dirent* entry;
            while ((entry = readdir(dir)) != nullptr) {
                if (entry->d_name[0] != '.') {
                    count++;
                }
            }
            closedir(dir);
        }
        return count;
    }

    mutable std::mutex metrics_mutex_;
    mutable std::mutex time_series_mutex_;
    std::atomic<bool> running_;
    std::thread monitor_thread_;
    ResourceMetrics baseline_metrics_;
    std::vector<ResourceMetrics> time_series_;
    int64_t peak_rss_kb_;
    int64_t total_cpu_time_ms_;
};

// SystemResourceMonitor 实现
SystemResourceMonitor::SystemResourceMonitor() : impl_(std::make_unique<Impl>()) {}
SystemResourceMonitor::~SystemResourceMonitor() = default;

void SystemResourceMonitor::Start() { impl_->Start(); }
void SystemResourceMonitor::Stop() { impl_->Stop(); }
SystemResourceMonitor::ResourceMetrics SystemResourceMonitor::GetCurrentMetrics() const {
    return impl_->GetCurrentMetrics();
}
std::vector<SystemResourceMonitor::ResourceMetrics> SystemResourceMonitor::GetTimeSeries() const {
    return impl_->GetTimeSeries();
}
int64_t SystemResourceMonitor::GetPeakMemoryKb() const { return impl_->GetPeakMemoryKb(); }
double SystemResourceMonitor::GetAverageCpuUsage() const { return impl_->GetAverageCpuUsage(); }

/**
 * @brief 内存基准测试实现
 *
 * 测量服务器在不同负载下的内存使用情况：
 * 1. 空闲状态内存使用
 * 2. 稳定负载下的内存使用
 * 3. 压力测试后的内存泄漏检测
 */
class MemoryBenchmark : public Benchmark {
public:
    std::string GetName() const override {
        return "memory_benchmark";
    }

    std::string GetDescription() const override {
        return "测量服务器内存使用情况和内存泄漏检测";
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

        // 阶段1：测量基线内存使用（服务器空闲）
        LOG_INFO("内存基准测试: 阶段1 - 测量基线内存使用");

        SystemResourceMonitor baseline_monitor;
        baseline_monitor.Start();

        // 启动服务器但不发送请求
        std::unique_ptr<Server> server;
        std::thread server_thread;

        try {
            server = std::make_unique<Server>(config.server_host, config.server_port, PluginManager::GetInstance());
            server_thread = std::thread([&server]() {
                server->Run();
            });

            std::this_thread::sleep_for(std::chrono::milliseconds(2000));
            LOG_INFO("内存基准测试: 服务器已启动（空闲状态）");
        } catch (const std::exception& e) {
            result.success = false;
            result.error_message = std::string("启动服务器失败: ") + e.what();
            result.end_time = std::chrono::system_clock::now();
            return result;
        }

        // 等待服务器稳定
        std::this_thread::sleep_for(std::chrono::seconds(2));
        baseline_monitor.Stop();

        auto baseline_metrics = baseline_monitor.GetCurrentMetrics();
        auto baseline_time_series = baseline_monitor.GetTimeSeries();

        // 阶段2：测量稳定负载下的内存使用
        LOG_INFO("内存基准测试: 阶段2 - 测量稳定负载内存使用");

        SystemResourceMonitor load_monitor;
        load_monitor.Start();

        // 运行稳定负载测试（简化版，复用QPS测试逻辑）
        auto load_test_result = RunLoadTest(config, std::chrono::seconds(10));
        load_monitor.Stop();

        auto load_metrics = load_monitor.GetCurrentMetrics();
        auto load_time_series = load_monitor.GetTimeSeries();

        // 阶段3：停止负载，测量内存回收
        LOG_INFO("内存基准测试: 阶段3 - 测量内存回收");

        // 停止负载测试
        std::this_thread::sleep_for(std::chrono::seconds(3));

        SystemResourceMonitor recovery_monitor;
        recovery_monitor.Start();
        std::this_thread::sleep_for(std::chrono::seconds(5));
        recovery_monitor.Stop();

        auto recovery_metrics = recovery_monitor.GetCurrentMetrics();
        auto recovery_time_series = recovery_monitor.GetTimeSeries();

        // 阶段4：停止服务器，最终清理
        LOG_INFO("内存基准测试: 阶段4 - 停止服务器");

        if (server) {
            server->Stop();
        }
        if (server_thread.joinable()) {
            server_thread.join();
        }

        // 等待所有资源释放
        std::this_thread::sleep_for(std::chrono::seconds(2));

        // 计算指标
        result.success = true;
        result.end_time = std::chrono::system_clock::now();
        result.duration_seconds = std::chrono::duration<double>(
            result.end_time - result.start_time).count();

        // 添加内存指标
        result.metrics.push_back({"baseline_rss_kb", static_cast<double>(baseline_metrics.rss_kb),
                                 "KB", "基线驻留内存"});
        result.metrics.push_back({"baseline_vm_size_kb", static_cast<double>(baseline_metrics.vm_size_kb),
                                 "KB", "基线虚拟内存"});

        result.metrics.push_back({"load_rss_kb", static_cast<double>(load_metrics.rss_kb),
                                 "KB", "负载下驻留内存"});
        result.metrics.push_back({"load_vm_size_kb", static_cast<double>(load_metrics.vm_size_kb),
                                 "KB", "负载下虚拟内存"});
        result.metrics.push_back({"load_peak_rss_kb", static_cast<double>(load_monitor.GetPeakMemoryKb()),
                                 "KB", "负载下峰值驻留内存"});
        result.metrics.push_back({"load_avg_cpu_usage", load_monitor.GetAverageCpuUsage(),
                                 "%", "负载下平均CPU使用率"});

        result.metrics.push_back({"recovery_rss_kb", static_cast<double>(recovery_metrics.rss_kb),
                                 "KB", "恢复后驻留内存"});
        result.metrics.push_back({"recovery_vm_size_kb", static_cast<double>(recovery_metrics.vm_size_kb),
                                 "KB", "恢复后虚拟内存"});

        // 计算内存增长
        double rss_growth_kb = static_cast<double>(load_metrics.rss_kb - baseline_metrics.rss_kb);
        double rss_growth_percent = (baseline_metrics.rss_kb > 0) ?
            (rss_growth_kb / baseline_metrics.rss_kb * 100) : 0;

        result.metrics.push_back({"rss_growth_kb", rss_growth_kb, "KB", "内存增长量"});
        result.metrics.push_back({"rss_growth_percent", rss_growth_percent, "%", "内存增长率"});

        // 内存泄漏检测指标
        double rss_leak_kb = static_cast<double>(recovery_metrics.rss_kb - baseline_metrics.rss_kb);
        double rss_leak_percent = (baseline_metrics.rss_kb > 0) ?
            (rss_leak_kb / baseline_metrics.rss_kb * 100) : 0;

        result.metrics.push_back({"potential_leak_kb", rss_leak_kb, "KB", "潜在内存泄漏"});
        result.metrics.push_back({"potential_leak_percent", rss_leak_percent, "%", "潜在内存泄漏率"});

        // 添加性能指标（从负载测试）
        if (load_test_result.success) {
            for (const auto& metric : load_test_result.metrics) {
                if (metric.name == "qps" || metric.name == "avg_latency" || metric.name == "error_rate") {
                    result.metrics.push_back({"load_" + metric.name, metric.value,
                                             metric.unit, "负载下" + metric.description});
                }
            }
        }

        // 添加时间序列数据（如果收集）
        if (config.collect_time_series) {
            // 合并所有时间序列
            for (const auto& ts : baseline_time_series) {
                BenchmarkResult::TimeSeriesPoint point;
                point.timestamp = ts.timestamp;
                point.value = static_cast<double>(ts.rss_kb);
                result.time_series.push_back(point);
            }
        }

        LOG_INFO("内存基准测试完成: 基线RSS=%ldKB, 负载RSS=%ldKB, 潜在泄漏=%ldKB",
                baseline_metrics.rss_kb, load_metrics.rss_kb, rss_leak_kb);

        return result;
    }

    void WarmUp(const BenchmarkConfig& /*config*/) override {
        // 内存测试不需要预热，因为我们要测量冷启动的内存使用
        LOG_INFO("内存基准测试: 跳过预热阶段");
    }

    void CleanUp(const BenchmarkConfig& /*config*/) override {
        LOG_INFO("内存基准测试: 清理完成");
    }

private:
    // 简化的负载测试（复用QPS测试逻辑）
    BenchmarkResult RunLoadTest(const BenchmarkConfig& config, std::chrono::seconds duration) {
        BenchmarkResult result;
        result.name = "load_test";
        result.start_time = std::chrono::system_clock::now();

        // 简化的HTTP客户端类
        class SimpleHttpClient {
        public:
            SimpleHttpClient(const std::string& host, int port) : host_(host), port_(port), fd_(-1) {
                Connect();
            }

            ~SimpleHttpClient() {
                if (fd_ >= 0) {
                    close(fd_);
                }
            }

            bool SendRequest(const std::string& path) {
                if (fd_ < 0 && !Connect()) {
                    return false;
                }

                std::string request = "GET " + path + " HTTP/1.1\r\n";
                request += "Host: " + host_ + "\r\n";
                request += "Connection: close\r\n";
                request += "\r\n";

                if (send(fd_, request.c_str(), request.size(), 0) < 0) {
                    CloseConnection();
                    return false;
                }

                // 简单接收（忽略响应内容）
                char buffer[1024];
                struct timeval tv;
                tv.tv_sec = 1;
                tv.tv_usec = 0;
                setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

                recv(fd_, buffer, sizeof(buffer), 0);
                CloseConnection();

                return true;
            }

        private:
            bool Connect() {
                fd_ = socket(AF_INET, SOCK_STREAM, 0);
                if (fd_ < 0) return false;

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
            int fd_;
        };

        // 运行负载测试
        std::atomic<int64_t> total_requests{0};
        std::atomic<int64_t> successful_requests{0};
        std::atomic<bool> stop_test{false};
        auto test_start_time = std::chrono::steady_clock::now();

        auto worker_func = [&](int worker_id) {
            while (!stop_test) {
                SimpleHttpClient client(config.server_host, config.server_port);
                if (client.SendRequest(config.request_path)) {
                    successful_requests++;
                }
                total_requests++;

                // 控制速率
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        };

        // 启动少量工作线程（避免过度消耗资源）
        const int num_workers = std::min(10, config.concurrent_connections);
        std::vector<std::thread> workers;
        for (int i = 0; i < num_workers; ++i) {
            workers.emplace_back(worker_func, i);
        }

        std::this_thread::sleep_for(duration);

        stop_test = true;
        for (auto& worker : workers) {
            if (worker.joinable()) {
                worker.join();
            }
        }

        auto test_end_time = std::chrono::steady_clock::now();
        double test_duration = std::chrono::duration<double>(test_end_time - test_start_time).count();

        // 填充结果
        result.success = true;
        result.end_time = std::chrono::system_clock::now();
        result.duration_seconds = test_duration;

        double qps = (test_duration > 0) ? successful_requests / test_duration : 0;
        double error_rate = (total_requests > 0) ?
            static_cast<double>(total_requests - successful_requests) / total_requests : 0;

        result.metrics.push_back({"qps", qps, "requests/second", "每秒查询数"});
        result.metrics.push_back({"total_requests", static_cast<double>(total_requests), "requests", "总请求数"});
        result.metrics.push_back({"successful_requests", static_cast<double>(successful_requests), "requests", "成功请求数"});
        result.metrics.push_back({"error_rate", error_rate * 100, "%", "错误率"});

        return result;
    }
};

// 工厂函数
std::unique_ptr<Benchmark> CreateMemoryBenchmark() {
    return std::make_unique<MemoryBenchmark>();
}

} // namespace benchmark
} // namespace tinywebserver