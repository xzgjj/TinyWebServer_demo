#pragma once

/**
 * @file benchmark.h
 * @brief 性能基准测试框架核心接口
 *
 * 提供统一的基准测试接口，支持不同类型的性能测试：
 * - QPS (每秒查询数) 测试
 * - 延迟分布测试
 * - 内存使用测试
 * - 并发能力测试
 *
 * 所有测试结果以JSON格式输出，便于自动化分析与对比。
 */

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace tinywebserver {
namespace benchmark {

/**
 * @brief 基准测试结果数据结构
 */
struct BenchmarkResult {
    /// 测试名称
    std::string name;

    /// 测试是否成功
    bool success;

    /// 错误信息（如果失败）
    std::string error_message;

    /// 测试持续时间（秒）
    double duration_seconds;

    /// 开始时间戳
    std::chrono::system_clock::time_point start_time;

    /// 结束时间戳
    std::chrono::system_clock::time_point end_time;

    /// 自定义指标（键值对）
    struct Metric {
        std::string name;
        double value;
        std::string unit;
        std::string description;
    };

    std::vector<Metric> metrics;

    /// 时间序列数据（用于绘制图表）
    struct TimeSeriesPoint {
        std::chrono::steady_clock::time_point timestamp;
        double value;
    };

    std::vector<TimeSeriesPoint> time_series;

    /// 转换为JSON字符串
    std::string ToJson() const;

    /// 从JSON字符串解析
    static BenchmarkResult FromJson(const std::string& json_str);
};

/**
 * @brief 基准测试配置
 */
struct BenchmarkConfig {
    /// 测试名称
    std::string name;

    /// 测试描述
    std::string description;

    /// 测试持续时间（秒），0表示运行一次
    double duration_seconds = 10.0;

    /// 并发连接数
    int concurrent_connections = 100;

    /// 目标QPS（0表示尽力而为）
    int target_qps = 0;

    /// 服务器地址
    std::string server_host = "127.0.0.1";

    /// 服务器端口
    int server_port = 8080;

    /// 请求路径
    std::string request_path = "/index.html";

    /// 请求方法
    std::string request_method = "GET";

    /// 请求体（如果有）
    std::string request_body;

    /// 是否启用Keep-Alive
    bool keep_alive = true;

    /// 是否收集详细的时间序列数据
    bool collect_time_series = false;

    /// 自定义配置参数
    std::vector<std::pair<std::string, std::string>> custom_params;

    /// 从JSON文件加载配置
    static BenchmarkConfig LoadFromFile(const std::string& file_path);

    /// 保存配置到JSON文件
    bool SaveToFile(const std::string& file_path) const;

    /// 转换为JSON字符串
    std::string ToJson() const;

    /// 验证配置有效性
    std::vector<std::string> Validate() const;
};

/**
 * @brief 基准测试基类（接口）
 */
class Benchmark {
public:
    virtual ~Benchmark() = default;

    /**
     * @brief 获取测试名称
     */
    virtual std::string GetName() const = 0;

    /**
     * @brief 获取测试描述
     */
    virtual std::string GetDescription() const = 0;

    /**
     * @brief 运行基准测试
     * @param config 测试配置
     * @return 测试结果
     */
    virtual BenchmarkResult Run(const BenchmarkConfig& config) = 0;

    /**
     * @brief 预热阶段（可选）
     * @param config 测试配置
     */
    virtual void WarmUp(const BenchmarkConfig& config) {}

    /**
     * @brief 清理阶段（可选）
     * @param config 测试配置
     */
    virtual void CleanUp(const BenchmarkConfig& config) {}
};

/**
 * @brief 基准测试工厂：创建特定类型的基准测试
 */
class BenchmarkFactory {
public:
    /**
     * @brief 创建基准测试实例
     * @param benchmark_type 测试类型 ("qps", "latency", "memory", "concurrent")
     * @return 基准测试实例指针，失败返回nullptr
     */
    static std::unique_ptr<Benchmark> Create(const std::string& benchmark_type);

    /**
     * @brief 获取所有支持的基准测试类型
     * @return 类型名称列表
     */
    static std::vector<std::string> GetAvailableTypes();
};

/**
 * @brief 系统资源监控器
 *
 * 监控测试过程中的系统资源使用情况：
 * - 内存使用（RSS, VmSize）
 * - CPU使用率
 * - 文件描述符数量
 * - 上下文切换次数
 */
class SystemResourceMonitor {
public:
    SystemResourceMonitor();
    ~SystemResourceMonitor();

    /**
     * @brief 开始监控
     */
    void Start();

    /**
     * @brief 停止监控
     */
    void Stop();

    /**
     * @brief 获取监控结果
     */
    struct ResourceMetrics {
        /// 驻留集大小（KB）
        int64_t rss_kb;
        /// 虚拟内存大小（KB）
        int64_t vm_size_kb;
        /// CPU使用时间（用户态+内核态，毫秒）
        int64_t cpu_time_ms;
        /// 文件描述符数量
        int fd_count;
        /// 上下文切换次数
        int64_t context_switches;
        /// 缺页次数
        int64_t page_faults;
        /// 系统时间戳
        std::chrono::steady_clock::time_point timestamp;
    };

    /// 获取当前资源使用情况
    ResourceMetrics GetCurrentMetrics() const;

    /// 获取整个监控期间的时间序列数据
    std::vector<ResourceMetrics> GetTimeSeries() const;

    /// 获取峰值内存使用（KB）
    int64_t GetPeakMemoryKb() const;

    /// 获取平均CPU使用率（百分比）
    double GetAverageCpuUsage() const;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

/**
 * @brief 统计计算工具类
 */
class Statistics {
public:
    /**
     * @brief 计算百分位数
     * @param values 数据值
     * @param percentile 百分位（0-100）
     * @return 百分位数
     */
    static double CalculatePercentile(const std::vector<double>& values, double percentile);

    /**
     * @brief 计算平均值
     */
    static double CalculateMean(const std::vector<double>& values);

    /**
     * @brief 计算标准差
     */
    static double CalculateStdDev(const std::vector<double>& values);

    /**
     * @brief 计算最小值
     */
    static double CalculateMin(const std::vector<double>& values);

    /**
     * @brief 计算最大值
     */
    static double CalculateMax(const std::vector<double>& values);

    /**
     * @brief 计算中位数
     */
    static double CalculateMedian(const std::vector<double>& values);

    /**
     * @brief 计算方差
     */
    static double CalculateVariance(const std::vector<double>& values);
};

} // namespace benchmark
} // namespace tinywebserver