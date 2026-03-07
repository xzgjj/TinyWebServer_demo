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
#include <nlohmann/json.hpp>
#include <iostream>
#include <fstream>
#include <string>
#include <vector>
#include <memory>
#include <cstdlib>
#include <ctime>
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
            csv_file << "name,value,unit,description\n";
            for (const auto& metric : result.metrics) {
                csv_file << "\"" << metric.name << "\","
                        << metric.value << ","
                        << "\"" << metric.unit << "\","
                        << "\"" << metric.description << "\"\n";
            }
            csv_file.close();
        }

        // 保存时间序列数据（如果存在）
        if (!result.time_series.empty()) {
            std::ofstream ts_file(output_dir + "/time_series.csv");
            if (ts_file.is_open()) {
                ts_file << "timestamp_ms,value\n";
                for (const auto& point : result.time_series) {
                    auto timestamp_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        point.timestamp.time_since_epoch()).count();
                    ts_file << timestamp_ms << "," << point.value << "\n";
                }
                ts_file.close();
            }
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
    std::cout << "  --type <type>          基准测试类型 (qps, latency, memory)\n";
    std::cout << "  --config <file>        配置文件路径\n";
    std::cout << "  --output <dir>         输出目录 (默认: benchmark_results/<timestamp>)\n";
    std::cout << "  --baseline <dir>       基线结果目录，用于对比\n";
    std::cout << "  --duration <seconds>   测试持续时间 (默认: 10)\n";
    std::cout << "  --connections <num>    并发连接数 (默认: 100)\n";
    std::cout << "  --host <host>          服务器地址 (默认: 127.0.0.1)\n";
    std::cout << "  --port <port>          服务器端口 (默认: 8080)\n\n";
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

    // 初始化日志
    Logger::GetInstance().Init("./benchmark_runner.log");
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
            } else if (arg == "--path" && i + 1 < argc) {
                config.request_path = argv[++i];
            } else if (arg == "--method" && i + 1 < argc) {
                config.request_method = argv[++i];
            } else if (arg == "--help" || arg == "-h") {
                PrintHelp();
                return 0;
            }
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