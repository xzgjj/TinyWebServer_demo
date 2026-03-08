/**
 * @file benchmark_runner.cpp
 * @brief 基准测试运行器主程序
 *
 * 命令行工具，支持：
 * 1. 运行特定类型的基准测试
 * 2. 从配置文件加载测试参数
 * 3. 保存测试结果到文件
 * 4. 与基线结果对比
 */

#include "../../include/benchmark.h"
#include "../../include/Logger.h"
#include "json.hpp"
#include <iostream>
#include <fstream>
#include <string>
#include <vector>
#include <memory>
#include <cstdlib>
#include <ctime>
#include <chrono>
#include <optional>
#include <iomanip>
#include <sstream>
#include <filesystem>
#include <unistd.h>

using json = nlohmann::json;
namespace fs = std::filesystem;

namespace tinywebserver {
namespace benchmark {

/**
 * @brief 获取当前Git提交哈希（简化版本）
 */
std::string GetGitCommitHash() {
    // 这里可以调用git命令获取提交哈希
    // 简化版：返回占位符
    return "unknown";
}

/**
 * @brief 获取当前时间戳字符串（用于目录名）
 */
std::string GetTimestampString() {
    auto now = std::chrono::system_clock::now();
    auto tt = std::chrono::system_clock::to_time_t(now);
    std::tm tm = *std::localtime(&tt);

    std::ostringstream oss;
    oss << std::put_time(&tm, "%Y-%m-%dT%H-%M-%S");
    return oss.str();
}

/**
 * @brief 获取环境信息
 */
json GetEnvironmentInfo() {
    json env;

    // CPU信息
    env["cpu_model"] = "Unknown";
    env["cpu_cores"] = std::thread::hardware_concurrency();

    // 内存信息
    long pages = sysconf(_SC_PHYS_PAGES);
    long page_size = sysconf(_SC_PAGE_SIZE);
    env["total_memory_mb"] = (pages * page_size) / (1024 * 1024);

    // 操作系统信息
    #ifdef __linux__
    env["os"] = "Linux";
    #elif __APPLE__
    env["os"] = "macOS";
    #else
    env["os"] = "Unknown";
    #endif

    // 编译器信息
    #ifdef __GNUC__
    env["compiler"] = "GCC " + std::to_string(__GNUC__) + "." + std::to_string(__GNUC_MINOR__) + "." + std::to_string(__GNUC_PATCHLEVEL__);
    #elif _MSC_VER
    env["compiler"] = "MSVC " + std::to_string(_MSC_VER);
    #else
    env["compiler"] = "Unknown";
    #endif

    // 构建类型
    #ifdef NDEBUG
    env["build_type"] = "Release";
    #else
    env["build_type"] = "Debug";
    #endif

    return env;
}

/**
 * @brief 时间戳转换为ISO格式字符串
 */
std::string TimeToString(const std::chrono::system_clock::time_point& tp) {
    auto tt = std::chrono::system_clock::to_time_t(tp);
    std::tm tm = *std::gmtime(&tt);
    std::ostringstream oss;
    oss << std::put_time(&tm, "%Y-%m-%dT%H:%M:%SZ");
    return oss.str();
}

/**
 * @brief 生成CSV文件的头部注释
 */
std::string GenerateCsvHeader(const BenchmarkResult& result, const BenchmarkConfig& config) {
    std::ostringstream oss;
    oss << "# Benchmark Results\n";
    oss << "# Name: " << result.name << "\n";
    oss << "# Start Time: " << TimeToString(result.start_time) << "\n";
    oss << "# End Time: " << TimeToString(result.end_time) << "\n";
    oss << "# Duration: " << result.duration_seconds << " seconds\n";
    oss << "# Concurrent Connections: " << config.concurrent_connections << "\n";
    oss << "# Target QPS: " << (config.target_qps > 0 ? std::to_string(config.target_qps) : "unlimited") << "\n";
    oss << "# Server: " << config.server_host << ":" << config.server_port << "\n";
    oss << "# Request Path: " << config.request_path << "\n";
    oss << "# Request Method: " << config.request_method << "\n";
    oss << "# Keep-Alive: " << (config.keep_alive ? "enabled" : "disabled") << "\n";
    oss << "#\n";
    return oss.str();
}

/**
 * @brief 生成Markdown格式的测试报告
 */
std::string GenerateMarkdownReport(const BenchmarkResult& result, const BenchmarkConfig& config) {
    std::ostringstream oss;

    // 报告标题
    oss << "# 基准测试报告\n\n";

    // 测试概览
    oss << "## 测试概览\n\n";
    oss << "- **测试名称**: " << result.name << "\n";
    oss << "- **开始时间**: " << TimeToString(result.start_time) << "\n";
    oss << "- **结束时间**: " << TimeToString(result.end_time) << "\n";
    oss << "- **持续时间**: " << std::fixed << std::setprecision(3) << result.duration_seconds << " 秒\n";
    oss << "- **并发连接数**: " << config.concurrent_connections << "\n";
    oss << "- **服务器**: " << config.server_host << ":" << config.server_port << "\n";
    oss << "- **请求路径**: " << config.request_path << "\n";
    oss << "- **测试状态**: " << (result.success ? "✅ 成功" : "❌ 失败") << "\n";
    if (!result.error_message.empty()) {
        oss << "- **错误信息**: " << result.error_message << "\n";
    }
    oss << "\n";

    // 关键指标
    oss << "## 关键性能指标\n\n";

    // 辅助函数：查找指标值
    auto find_metric = [&](const std::string& name) -> std::optional<double> {
        for (const auto& metric : result.metrics) {
            if (metric.name == name) {
                return metric.value;
            }
        }
        return std::nullopt;
    };

    auto qps = find_metric("qps");
    auto error_rate = find_metric("error_rate");
    auto avg_latency = find_metric("avg_latency");
    auto p99_latency = find_metric("p99_latency");
    auto throughput_mbps = find_metric("throughput_mbps");

    oss << "| 指标 | 值 | 单位 | 描述 |\n";
    oss << "|------|----|------|------|\n";
    if (qps) {
        oss << "| QPS | " << std::fixed << std::setprecision(2) << *qps << " | requests/second | 每秒查询数 |\n";
    }
    if (error_rate) {
        oss << "| 错误率 | " << std::fixed << std::setprecision(2) << *error_rate << " | % | 请求错误率 |\n";
    }
    if (avg_latency) {
        oss << "| 平均延迟 | " << std::fixed << std::setprecision(2) << *avg_latency << " | ms | 平均响应时间 |\n";
    }
    if (p99_latency) {
        oss << "| P99延迟 | " << std::fixed << std::setprecision(2) << *p99_latency << " | ms | 99%分位响应时间 |\n";
    }
    if (throughput_mbps) {
        oss << "| 网络吞吐量 | " << std::fixed << std::setprecision(3) << *throughput_mbps << " | Mbps | 网络带宽使用 |\n";
    }
    oss << "\n";

    // 延迟分布摘要
    auto p50 = find_metric("p50_latency");
    auto p90 = find_metric("p90_latency");
    auto p95 = find_metric("p95_latency");
    auto p999 = find_metric("p999_latency");
    auto min_lat = find_metric("min_latency");
    auto max_lat = find_metric("max_latency");
    auto stddev_lat = find_metric("stddev_latency");

    if (p50 && p90 && p95 && p99_latency && p999) {
        oss << "## 延迟分布\n\n";
        oss << "- **P50**: " << *p50 << " ms\n";
        oss << "- **P90**: " << *p90 << " ms\n";
        oss << "- **P95**: " << *p95 << " ms\n";
        oss << "- **P99**: " << *p99_latency << " ms\n";
        oss << "- **P99.9**: " << *p999 << " ms\n";
        if (min_lat && max_lat) {
            oss << "- **范围**: " << *min_lat << " - " << *max_lat << " ms\n";
        }
        if (stddev_lat) {
            oss << "- **标准差**: " << *stddev_lat << " ms\n";
        }
        oss << "\n";
    }

    // 请求统计
    auto total_requests = find_metric("total_requests");
    auto successful_requests = find_metric("successful_requests");
    auto failed_requests = find_metric("failed_requests");

    if (total_requests && successful_requests && failed_requests) {
        oss << "## 请求统计\n\n";
        oss << "- **总请求数**: " << static_cast<int>(*total_requests) << "\n";
        oss << "- **成功请求**: " << static_cast<int>(*successful_requests) << "\n";
        oss << "- **失败请求**: " << static_cast<int>(*failed_requests) << "\n";
        oss << "- **成功率**: " << std::fixed << std::setprecision(2)
            << ((*total_requests > 0) ? (*successful_requests / *total_requests * 100.0) : 0.0) << "%\n";
        oss << "\n";
    }

    // 性能评估
    oss << "## 性能评估\n\n";

    bool excellent = false;
    bool good = false;
    bool warning = false;

    if (error_rate && *error_rate < 0.1) {
        if (qps && *qps > 1000) {
            excellent = true;
            oss << "✅ **优秀性能**: 高吞吐量，低错误率，适合生产环境。\n";
        } else if (qps && *qps > 100) {
            good = true;
            oss << "✓ **良好性能**: 中等吞吐量，低错误率，满足一般需求。\n";
        } else {
            oss << "⚠ **基础性能**: 低吞吐量，但错误率低，适合测试环境。\n";
        }
    } else if (error_rate && *error_rate < 5.0) {
        warning = true;
        oss << "⚠ **需要注意**: 错误率较高，建议检查服务器状态。\n";
    } else {
        oss << "❌ **性能问题**: 高错误率，需要立即排查。\n";
    }

    if (p99_latency && *p99_latency > 1000) {
        oss << "⚠ **高延迟警告**: P99延迟超过1秒，可能影响用户体验。\n";
    } else if (p99_latency && *p99_latency > 100) {
        oss << "⚠ **中等延迟**: P99延迟超过100毫秒，建议优化。\n";
    }

    oss << "\n";

    // 建议
    oss << "## 建议\n\n";
    if (excellent) {
        oss << "- 当前配置性能优秀，无需调整。\n";
        oss << "- 可以考虑增加并发连接数以测试极限性能。\n";
    } else if (good) {
        oss << "- 性能良好，可满足大多数应用场景。\n";
        oss << "- 如需更高吞吐量，可优化服务器配置或代码。\n";
    } else if (warning) {
        oss << "- 检查服务器资源使用情况（CPU、内存）。\n";
        oss << "- 检查网络连接和带宽限制。\n";
        oss << "- 考虑调整并发连接数或请求频率。\n";
    } else {
        oss << "- 立即检查服务器日志和错误信息。\n";
        oss << "- 验证服务器配置和资源限制。\n";
        oss << "- 降低并发连接数进行测试。\n";
    }

    // 文件列表
    oss << "## 生成文件\n\n";
    oss << "- `result.json`: 完整JSON格式结果\n";
    oss << "- `metadata.json`: 测试元数据\n";
    oss << "- `metrics.csv`: 指标CSV文件\n";
    oss << "- `metrics_grouped.csv`: 分组指标CSV文件\n";
    if (!result.time_series.empty()) {
        oss << "- `time_series.csv`: 时间序列数据\n";
    }
    oss << "- `report.md`: 本报告文件\n";

    oss << "\n---\n\n";
    oss << "*报告生成时间: " << TimeToString(std::chrono::system_clock::now()) << "*\n";
    oss << "*使用 TinyWebServer 基准测试框架生成*\n";

    return oss.str();
}

/**
 * @brief 保存测试结果到文件
 */
bool SaveBenchmarkResult(const BenchmarkResult& result, const std::string& output_dir,
                        const BenchmarkConfig& config, const json& env_info) {
    try {
        // 创建输出目录
        fs::create_directories(output_dir);

        // 保存主结果
        std::string result_json = result.ToJson();
        std::ofstream result_file(output_dir + "/result.json");
        if (!result_file.is_open()) {
            std::cerr << "无法打开结果文件: " << output_dir << "/result.json" << std::endl;
            return false;
        }
        result_file << result_json;
        result_file.close();

        // 保存元数据
        json metadata;
        metadata["timestamp"] = GetTimestampString();
        metadata["git_commit"] = GetGitCommitHash();
        metadata["benchmark_name"] = result.name;
        metadata["benchmark_config"] = json::parse(config.ToJson());
        metadata["environment"] = env_info;

        std::ofstream metadata_file(output_dir + "/metadata.json");
        if (!metadata_file.is_open()) {
            std::cerr << "无法打开元数据文件: " << output_dir << "/metadata.json" << std::endl;
            return false;
        }
        metadata_file << metadata.dump(2);
        metadata_file.close();

        // 保存原始指标为CSV（便于Excel导入）
        std::ofstream csv_file(output_dir + "/metrics.csv");
        if (csv_file.is_open()) {
            csv_file << GenerateCsvHeader(result, config);
            csv_file << result.ToCsv();
            csv_file.close();
        }

        // 保存时间序列数据（如果存在）
        if (!result.time_series.empty()) {
            std::ofstream ts_file(output_dir + "/time_series.csv");
            if (ts_file.is_open()) {
                ts_file << "# Time Series Data\n";
                ts_file << "# Benchmark: " << result.name << "\n";
                ts_file << "# Start Time: " << TimeToString(result.start_time) << "\n";
                ts_file << "# End Time: " << TimeToString(result.end_time) << "\n";
                ts_file << "# Data Points: " << result.time_series.size() << "\n";
                ts_file << "#\n";
                ts_file << result.ToTimeSeriesCsv();
                ts_file.close();
            }
        }

        // 增强：保存分组指标CSV
        std::ofstream grouped_csv_file(output_dir + "/metrics_grouped.csv");
        if (grouped_csv_file.is_open()) {
            grouped_csv_file << GenerateCsvHeader(result, config);
            grouped_csv_file << "group,name,value,unit,description\n";

            // 使用与ToCsv相同的分类逻辑
            auto classify_metric = [](const std::string& name) -> std::string {
                if (name.find("error") != std::string::npos ||
                    name.find("failed") != std::string::npos) {
                    return "errors";
                } else if (name.find("latency") != std::string::npos ||
                           name.find("_latency") != std::string::npos ||
                           name.find("delay") != std::string::npos) {
                    return "latency";
                } else if (name.find("qps") != std::string::npos ||
                           name.find("throughput") != std::string::npos ||
                           name.find("_requests") != std::string::npos) {
                    return "throughput";
                } else if (name.find("concurrent") != std::string::npos ||
                           name.find("connection") != std::string::npos) {
                    return "concurrency";
                } else if (name.find("memory") != std::string::npos ||
                           name.find("cpu") != std::string::npos ||
                           name.find("rss") != std::string::npos ||
                           name.find("vm") != std::string::npos) {
                    return "resources";
                }
                return "other";
            };

            for (const auto& metric : result.metrics) {
                std::string group = classify_metric(metric.name);

                // CSV转义
                std::string escaped_name = metric.name;
                std::string escaped_description = metric.description;

                if (escaped_name.find(',') != std::string::npos ||
                    escaped_name.find('"') != std::string::npos) {
                    size_t pos = 0;
                    while ((pos = escaped_name.find('"', pos)) != std::string::npos) {
                        escaped_name.replace(pos, 1, "\"\"");
                        pos += 2;
                    }
                    escaped_name = "\"" + escaped_name + "\"";
                }

                if (escaped_description.find(',') != std::string::npos ||
                    escaped_description.find('"') != std::string::npos) {
                    size_t pos = 0;
                    while ((pos = escaped_description.find('"', pos)) != std::string::npos) {
                        escaped_description.replace(pos, 1, "\"\"");
                        pos += 2;
                    }
                    escaped_description = "\"" + escaped_description + "\"";
                }

                grouped_csv_file << group << ","
                               << escaped_name << ","
                               << metric.value << ","
                               << metric.unit << ","
                               << escaped_description << "\n";
            }
            grouped_csv_file.close();
        }

        // 保存Markdown报告
        std::ofstream report_file(output_dir + "/report.md");
        if (report_file.is_open()) {
            report_file << GenerateMarkdownReport(result, config);
            report_file.close();
        }

        return true;
    } catch (const std::exception& e) {
        std::cerr << "保存结果时发生错误: " << e.what() << std::endl;
        return false;
    }
}

/**
 * @brief 与基线结果对比
 */
json CompareWithBaseline(const BenchmarkResult& current, const std::string& baseline_dir) {
    json comparison;
    comparison["baseline_dir"] = baseline_dir;

    try {
        // 加载基线结果
        std::ifstream baseline_file(baseline_dir + "/result.json");
        if (!baseline_file.is_open()) {
            comparison["error"] = "无法打开基线结果文件";
            return comparison;
        }

        std::string baseline_json_str;
        baseline_file.seekg(0, std::ios::end);
        baseline_json_str.reserve(baseline_file.tellg());
        baseline_file.seekg(0, std::ios::beg);
        baseline_json_str.assign((std::istreambuf_iterator<char>(baseline_file)),
                                std::istreambuf_iterator<char>());

        auto baseline_result = BenchmarkResult::FromJson(baseline_json_str);
        if (!baseline_result.success) {
            comparison["error"] = "基线结果解析失败: " + baseline_result.error_message;
            return comparison;
        }

        // 对比指标
        json metrics_comparison = json::array();

        // 将当前指标转换为map便于查找
        std::unordered_map<std::string, BenchmarkResult::Metric> current_metrics_map;
        for (const auto& metric : current.metrics) {
            current_metrics_map[metric.name] = metric;
        }

        // 对比每个基线指标
        for (const auto& baseline_metric : baseline_result.metrics) {
            auto it = current_metrics_map.find(baseline_metric.name);
            if (it != current_metrics_map.end()) {
                const auto& current_metric = it->second;

                double baseline_value = baseline_metric.value;
                double current_value = current_metric.value;
                double diff = current_value - baseline_value;
                double diff_percent = (baseline_value != 0) ? (diff / baseline_value * 100) : 0;

                json metric_comparison;
                metric_comparison["name"] = baseline_metric.name;
                metric_comparison["baseline_value"] = baseline_value;
                metric_comparison["current_value"] = current_value;
                metric_comparison["absolute_diff"] = diff;
                metric_comparison["relative_diff_percent"] = diff_percent;
                metric_comparison["unit"] = baseline_metric.unit;
                metric_comparison["improvement"] = (diff_percent > 0 && baseline_metric.name.find("latency") == std::string::npos) ?
                                                  "improved" : "regressed";

                metrics_comparison.push_back(metric_comparison);
            }
        }

        comparison["metrics_comparison"] = metrics_comparison;
        comparison["summary"] = {
            {"current_success", current.success},
            {"baseline_success", baseline_result.success},
            {"total_metrics_compared", metrics_comparison.size()}
        };

    } catch (const std::exception& e) {
        comparison["error"] = std::string("对比过程中发生错误: ") + e.what();
    }

    return comparison;
}

/**
 * @brief 打印帮助信息
 */
void PrintHelp() {
    std::cout << "TinyWebServer 性能基准测试运行器\n\n";
    std::cout << "用法:\n";
    std::cout << "  benchmark_runner <command> [options]\n\n";
    std::cout << "命令:\n";
    std::cout << "  run      运行基准测试\n";
    std::cout << "  compare  与基线结果对比\n";
    std::cout << "  list     列出可用的基准测试类型\n";
    std::cout << "  help     显示此帮助信息\n\n";
    std::cout << "运行命令选项:\n";
    std::cout << "  --type <type>          基准测试类型 (qps, latency, memory, concurrent)\n";
    std::cout << "  --config <file>        配置文件路径\n";
    std::cout << "  --output <dir>         输出目录 (默认: benchmark_results/<timestamp>)\n";
    std::cout << "  --baseline <dir>       基线结果目录，用于对比\n";
    std::cout << "  --duration <seconds>   测试持续时间 (默认: 10)\n";
    std::cout << "  --connections <num>    并发连接数 (默认: 100)\n";
    std::cout << "  --host <host>          服务器地址 (默认: 127.0.0.1)\n";
    std::cout << "  --port <port>          服务器端口 (默认: 8080)\n";
    std::cout << "  --path <path>          请求路径 (默认: /index.html)\n";
    std::cout << "  --method <method>      请求方法 (默认: GET)\n\n";
    std::cout << "示例:\n";
    std::cout << "  benchmark_runner run --type qps --duration 30 --connections 1000\n";
    std::cout << "  benchmark_runner run --config configs/benchmark/qps_config.json\n";
    std::cout << "  benchmark_runner compare --baseline benchmark_results/baseline\n";
}

} // namespace benchmark
} // namespace tinywebserver

// 声明工厂函数（在单独的工厂文件中实现）
namespace tinywebserver {
namespace benchmark {
std::unique_ptr<Benchmark> CreateBenchmark(const std::string& type);
std::vector<std::string> GetAvailableBenchmarkTypes();
} // namespace benchmark
} // namespace tinywebserver

int main(int argc, char* argv[]) {
    using namespace tinywebserver::benchmark;

    // 初始化日志，使用INFO级别以减少日志量（DEBUG级别会产生大量输出）
    Logger::GetInstance().Init("./local/logs/benchmark_runner.log", LogLevel::LOG_LEVEL_INFO);
    LOG_INFO("基准测试运行器启动");

    if (argc < 2) {
        PrintHelp();
        return 1;
    }

    std::string command = argv[1];

    if (command == "help" || command == "-h" || command == "--help") {
        PrintHelp();
        return 0;
    }

    if (command == "list") {
        std::cout << "可用的基准测试类型:\n";
        auto types = GetAvailableBenchmarkTypes();
        for (const auto& type : types) {
            std::cout << "  - " << type << "\n";
        }
        return 0;
    }

    if (command == "run") {
        // 解析命令行参数
        BenchmarkConfig config;
        std::string benchmark_type = "qps";
        std::string config_file;
        std::string output_dir;
        std::string baseline_dir;
        bool port_specified = false;

        for (int i = 2; i < argc; ++i) {
            std::string arg = argv[i];

            if (arg == "--type" && i + 1 < argc) {
                benchmark_type = argv[++i];
            } else if (arg == "--config" && i + 1 < argc) {
                config_file = argv[++i];
            } else if (arg == "--output" && i + 1 < argc) {
                output_dir = argv[++i];
            } else if (arg == "--baseline" && i + 1 < argc) {
                baseline_dir = argv[++i];
            } else if (arg == "--duration" && i + 1 < argc) {
                config.duration_seconds = std::stod(argv[++i]);
            } else if (arg == "--connections" && i + 1 < argc) {
                config.concurrent_connections = std::stoi(argv[++i]);
            } else if (arg == "--host" && i + 1 < argc) {
                config.server_host = argv[++i];
            } else if (arg == "--port" && i + 1 < argc) {
                config.server_port = std::stoi(argv[++i]);
                port_specified = true;
            } else if (arg == "--path" && i + 1 < argc) {
                config.request_path = argv[++i];
            } else if (arg == "--method" && i + 1 < argc) {
                config.request_method = argv[++i];
            } else if (arg == "--help" || arg == "-h") {
                PrintHelp();
                return 0;
            }
        }

        // 如果没有指定端口且当前为默认端口8080，改为8081以避免冲突
        if (!port_specified && config.server_port == 8080) {
            config.server_port = 8081;
            LOG_INFO("使用默认端口8081以避免冲突");
        }

        // 如果提供了配置文件，从文件加载配置
        if (!config_file.empty()) {
            config = BenchmarkConfig::LoadFromFile(config_file);
            if (config.name.empty() || config.name == "error") {
                std::cerr << "无法加载配置文件: " << config_file << std::endl;
                return 1;
            }
        } else {
            // 使用命令行参数填充默认配置
            config.name = benchmark_type + "_benchmark";
            config.description = "命令行运行的" + benchmark_type + "基准测试";
        }

        // 创建输出目录
        if (output_dir.empty()) {
            std::string timestamp = GetTimestampString();
            std::string commit_hash = GetGitCommitHash().substr(0, 8);
            output_dir = "benchmark_results/" + timestamp + "_" + commit_hash;
        }

        // 创建基准测试实例
        auto benchmark = CreateBenchmark(benchmark_type);
        if (!benchmark) {
            std::cerr << "未知的基准测试类型: " << benchmark_type << std::endl;
            std::cerr << "使用 'benchmark_runner list' 查看可用类型" << std::endl;
            return 1;
        }

        // 验证配置
        auto errors = config.Validate();
        if (!errors.empty()) {
            std::cerr << "配置验证失败:\n";
            for (const auto& error : errors) {
                std::cerr << "  - " << error << "\n";
            }
            return 1;
        }

        // 打印测试信息
        std::cout << "=========================================\n";
        std::cout << "运行基准测试: " << benchmark->GetName() << "\n";
        std::cout << "描述: " << benchmark->GetDescription() << "\n";
        std::cout << "配置:\n";
        std::cout << "  持续时间: " << config.duration_seconds << " 秒\n";
        std::cout << "  并发连接: " << config.concurrent_connections << "\n";
        std::cout << "  服务器: " << config.server_host << ":" << config.server_port << "\n";
        std::cout << "  请求路径: " << config.request_path << "\n";
        std::cout << "  输出目录: " << output_dir << "\n";
        if (!baseline_dir.empty()) {
            std::cout << "  基线目录: " << baseline_dir << "\n";
        }
        std::cout << "=========================================\n\n";

        // 运行基准测试
        LOG_INFO("开始运行基准测试: %s", benchmark->GetName().c_str());
        auto result = benchmark->Run(config);

        // 获取环境信息
        auto env_info = GetEnvironmentInfo();

        // 保存结果
        if (!SaveBenchmarkResult(result, output_dir, config, env_info)) {
            std::cerr << "保存结果失败" << std::endl;
            return 1;
        }

        // 打印结果摘要
        std::cout << "\n基准测试完成!\n";
        std::cout << "结果已保存到: " << output_dir << "\n\n";

        if (result.success) {
            std::cout << "关键指标:\n";
            for (const auto& metric : result.metrics) {
                if (metric.name == "qps" || metric.name == "avg_latency" ||
                    metric.name == "p99_latency" || metric.name == "error_rate") {
                    std::cout << "  " << metric.name << ": " << metric.value << " " << metric.unit << "\n";
                }
            }

            // 与基线对比
            if (!baseline_dir.empty()) {
                std::cout << "\n与基线对比:\n";
                auto comparison = CompareWithBaseline(result, baseline_dir);

                if (comparison.contains("error")) {
                    std::cout << "  对比失败: " << comparison["error"] << "\n";
                } else if (comparison.contains("metrics_comparison")) {
                    for (const auto& item : comparison["metrics_comparison"]) {
                        std::string name = item["name"];
                        double baseline = item["baseline_value"];
                        double current = item["current_value"];
                        double diff_percent = item["relative_diff_percent"];
                        std::string improvement = item["improvement"];

                        std::cout << "  " << name << ": " << current << " vs " << baseline
                                 << " (" << (diff_percent > 0 ? "+" : "") << diff_percent << "%) "
                                 << improvement << "\n";
                    }
                }
            }

            std::cout << "\n详细结果请查看: " << output_dir << "/result.json\n";

        } else {
            std::cerr << "基准测试失败: " << result.error_message << std::endl;
            return 1;
        }

        return 0;
    }

    if (command == "compare") {
        if (argc < 4) {
            std::cerr << "用法: benchmark_runner compare --baseline <基线目录> --current <当前目录>\n";
            return 1;
        }

        std::string baseline_dir;
        std::string current_dir;

        for (int i = 2; i < argc; ++i) {
            std::string arg = argv[i];
            if (arg == "--baseline" && i + 1 < argc) {
                baseline_dir = argv[++i];
            } else if (arg == "--current" && i + 1 < argc) {
                current_dir = argv[++i];
            }
        }

        if (baseline_dir.empty() || current_dir.empty()) {
            std::cerr << "需要指定基线目录和当前目录\n";
            return 1;
        }

        // 加载当前结果
        std::ifstream current_file(current_dir + "/result.json");
        if (!current_file.is_open()) {
            std::cerr << "无法打开当前结果文件: " << current_dir << "/result.json\n";
            return 1;
        }

        std::string current_json_str;
        current_file.seekg(0, std::ios::end);
        current_json_str.reserve(current_file.tellg());
        current_file.seekg(0, std::ios::beg);
        current_json_str.assign((std::istreambuf_iterator<char>(current_file)),
                               std::istreambuf_iterator<char>());

        auto current_result = BenchmarkResult::FromJson(current_json_str);
        if (!current_result.success) {
            std::cerr << "当前结果解析失败: " << current_result.error_message << "\n";
            return 1;
        }

        // 对比
        auto comparison = CompareWithBaseline(current_result, baseline_dir);

        if (comparison.contains("error")) {
            std::cerr << "对比失败: " << comparison["error"] << "\n";
            return 1;
        }

        std::cout << "性能对比报告\n";
        std::cout << "=========================================\n";
        std::cout << "基线目录: " << baseline_dir << "\n";
        std::cout << "当前目录: " << current_dir << "\n\n";

        if (comparison.contains("metrics_comparison")) {
            std::cout << "指标对比:\n";
            for (const auto& item : comparison["metrics_comparison"]) {
                std::string name = item["name"];
                double baseline = item["baseline_value"];
                double current = item["current_value"];
                double diff = item["absolute_diff"];
                double diff_percent = item["relative_diff_percent"];
                std::string unit = item["unit"];
                std::string improvement = item["improvement"];

                std::cout << "  " << name << ":\n";
                std::cout << "    基线: " << baseline << " " << unit << "\n";
                std::cout << "    当前: " << current << " " << unit << "\n";
                std::cout << "    变化: " << (diff > 0 ? "+" : "") << diff << " " << unit
                         << " (" << (diff_percent > 0 ? "+" : "") << diff_percent << "%)\n";
                std::cout << "    状态: " << improvement << "\n\n";
            }
        }

        return 0;
    }

    std::cerr << "未知命令: " << command << "\n";
    std::cerr << "使用 'benchmark_runner help' 查看帮助\n";
    return 1;
}