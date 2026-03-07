/**
 * @file benchmark_base.cpp
 * @brief 基准测试基础实现
 */

#include "../../include/benchmark.h"
#include <nlohmann/json.hpp>
#include <fstream>
#include <iostream>
#include <sstream>
#include <iomanip>
#include <cmath>

using json = nlohmann::json;

namespace tinywebserver {
namespace benchmark {

// BenchmarkResult 方法实现
std::string BenchmarkResult::ToJson() const {
    json j;
    j["name"] = name;
    j["success"] = success;
    j["error_message"] = error_message;
    j["duration_seconds"] = duration_seconds;

    // 时间戳转换为ISO格式字符串
    auto time_to_string = [](const std::chrono::system_clock::time_point& tp) -> std::string {
        auto tt = std::chrono::system_clock::to_time_t(tp);
        std::tm tm = *std::gmtime(&tt);
        std::ostringstream oss;
        oss << std::put_time(&tm, "%Y-%m-%dT%H:%M:%SZ");
        return oss.str();
    };

    j["start_time"] = time_to_string(start_time);
    j["end_time"] = time_to_string(end_time);

    // 指标
    json metrics_json = json::array();
    for (const auto& metric : metrics) {
        json m;
        m["name"] = metric.name;
        m["value"] = metric.value;
        m["unit"] = metric.unit;
        m["description"] = metric.description;
        metrics_json.push_back(m);
    }
    j["metrics"] = metrics_json;

    // 时间序列
    json ts_json = json::array();
    for (const auto& point : time_series) {
        json p;
        auto timestamp_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            point.timestamp.time_since_epoch()).count();
        p["timestamp_ms"] = timestamp_ms;
        p["value"] = point.value;
        ts_json.push_back(p);
    }
    j["time_series"] = ts_json;

    return j.dump(2); // 缩进2空格，美化输出
}

BenchmarkResult BenchmarkResult::FromJson(const std::string& json_str) {
    BenchmarkResult result;
    try {
        json j = json::parse(json_str);

        result.name = j.value("name", "");
        result.success = j.value("success", false);
        result.error_message = j.value("error_message", "");
        result.duration_seconds = j.value("duration_seconds", 0.0);

        // 解析时间戳
        auto string_to_time = [](const std::string& str) -> std::chrono::system_clock::time_point {
            std::tm tm = {};
            std::istringstream iss(str);
            iss >> std::get_time(&tm, "%Y-%m-%dT%H:%M:%SZ");
            auto tt = std::mktime(&tm);
            // 注意：mktime 使用本地时间，但这里假设是UTC
            return std::chrono::system_clock::from_time_t(tt);
        };

        if (j.contains("start_time")) {
            result.start_time = string_to_time(j["start_time"].get<std::string>());
        }
        if (j.contains("end_time")) {
            result.end_time = string_to_time(j["end_time"].get<std::string>());
        }

        // 解析指标
        if (j.contains("metrics") && j["metrics"].is_array()) {
            for (const auto& m : j["metrics"]) {
                Metric metric;
                metric.name = m.value("name", "");
                metric.value = m.value("value", 0.0);
                metric.unit = m.value("unit", "");
                metric.description = m.value("description", "");
                result.metrics.push_back(metric);
            }
        }

        // 解析时间序列
        if (j.contains("time_series") && j["time_series"].is_array()) {
            for (const auto& p : j["time_series"]) {
                TimeSeriesPoint point;
                auto timestamp_ms = p.value("timestamp_ms", int64_t(0));
                point.timestamp = std::chrono::steady_clock::time_point(
                    std::chrono::milliseconds(timestamp_ms));
                point.value = p.value("value", 0.0);
                result.time_series.push_back(point);
            }
        }
    } catch (const json::exception& e) {
        result.success = false;
        result.error_message = std::string("JSON解析失败: ") + e.what();
    }

    return result;
}

// BenchmarkConfig 方法实现
BenchmarkConfig BenchmarkConfig::LoadFromFile(const std::string& file_path) {
    BenchmarkConfig config;
    try {
        std::ifstream file(file_path);
        if (!file.is_open()) {
            config.name = "error";
            return config;
        }

        json j;
        file >> j;

        config.name = j.value("name", "");
        config.description = j.value("description", "");
        config.duration_seconds = j.value("duration_seconds", 10.0);
        config.concurrent_connections = j.value("concurrent_connections", 100);
        config.target_qps = j.value("target_qps", 0);
        config.server_host = j.value("server_host", "127.0.0.1");
        config.server_port = j.value("server_port", 8080);
        config.request_path = j.value("request_path", "/index.html");
        config.request_method = j.value("request_method", "GET");
        config.request_body = j.value("request_body", "");
        config.keep_alive = j.value("keep_alive", true);
        config.collect_time_series = j.value("collect_time_series", false);

        if (j.contains("custom_params") && j["custom_params"].is_object()) {
            for (auto& item : j["custom_params"].items()) {
                config.custom_params.emplace_back(item.key(), item.value().get<std::string>());
            }
        }
    } catch (const json::exception& e) {
        std::cerr << "加载配置文件失败: " << e.what() << std::endl;
    }

    return config;
}

bool BenchmarkConfig::SaveToFile(const std::string& file_path) const {
    try {
        std::ofstream file(file_path);
        if (!file.is_open()) {
            return false;
        }

        json j;
        j["name"] = name;
        j["description"] = description;
        j["duration_seconds"] = duration_seconds;
        j["concurrent_connections"] = concurrent_connections;
        j["target_qps"] = target_qps;
        j["server_host"] = server_host;
        j["server_port"] = server_port;
        j["request_path"] = request_path;
        j["request_method"] = request_method;
        j["request_body"] = request_body;
        j["keep_alive"] = keep_alive;
        j["collect_time_series"] = collect_time_series;

        json custom_params_json = json::object();
        for (const auto& param : custom_params) {
            custom_params_json[param.first] = param.second;
        }
        j["custom_params"] = custom_params_json;

        file << j.dump(2);
        return true;
    } catch (const json::exception& e) {
        std::cerr << "保存配置文件失败: " << e.what() << std::endl;
        return false;
    }
}

std::string BenchmarkConfig::ToJson() const {
    json j;
    j["name"] = name;
    j["description"] = description;
    j["duration_seconds"] = duration_seconds;
    j["concurrent_connections"] = concurrent_connections;
    j["target_qps"] = target_qps;
    j["server_host"] = server_host;
    j["server_port"] = server_port;
    j["request_path"] = request_path;
    j["request_method"] = request_method;
    j["request_body"] = request_body;
    j["keep_alive"] = keep_alive;
    j["collect_time_series"] = collect_time_series;

    json custom_params_json = json::object();
    for (const auto& param : custom_params) {
        custom_params_json[param.first] = param.second;
    }
    j["custom_params"] = custom_params_json;

    return j.dump(2);
}

std::vector<std::string> BenchmarkConfig::Validate() const {
    std::vector<std::string> errors;

    if (name.empty()) {
        errors.push_back("测试名称不能为空");
    }

    if (duration_seconds < 0) {
        errors.push_back("测试持续时间不能为负数");
    }

    if (concurrent_connections <= 0) {
        errors.push_back("并发连接数必须大于0");
    }

    if (server_host.empty()) {
        errors.push_back("服务器地址不能为空");
    }

    if (server_port <= 0 || server_port > 65535) {
        errors.push_back("服务器端口号无效");
    }

    if (request_path.empty()) {
        errors.push_back("请求路径不能为空");
    }

    return errors;
}

// Statistics 方法实现
double Statistics::CalculatePercentile(const std::vector<double>& values, double percentile) {
    if (values.empty()) {
        return 0.0;
    }

    if (percentile <= 0) {
        return values.front();
    }

    if (percentile >= 100) {
        return values.back();
    }

    std::vector<double> sorted_values = values;
    std::sort(sorted_values.begin(), sorted_values.end());

    double index = (percentile / 100.0) * (sorted_values.size() - 1);
    int lower_index = static_cast<int>(std::floor(index));
    int upper_index = static_cast<int>(std::ceil(index));

    if (lower_index == upper_index) {
        return sorted_values[lower_index];
    }

    double weight = index - lower_index;
    return sorted_values[lower_index] * (1 - weight) + sorted_values[upper_index] * weight;
}

double Statistics::CalculateMean(const std::vector<double>& values) {
    if (values.empty()) {
        return 0.0;
    }

    double sum = 0.0;
    for (double value : values) {
        sum += value;
    }
    return sum / values.size();
}

double Statistics::CalculateStdDev(const std::vector<double>& values) {
    if (values.empty()) {
        return 0.0;
    }

    double mean = CalculateMean(values);
    double sum_squared_diff = 0.0;
    for (double value : values) {
        double diff = value - mean;
        sum_squared_diff += diff * diff;
    }

    double variance = sum_squared_diff / values.size();
    return std::sqrt(variance);
}

double Statistics::CalculateMin(const std::vector<double>& values) {
    if (values.empty()) {
        return 0.0;
    }

    return *std::min_element(values.begin(), values.end());
}

double Statistics::CalculateMax(const std::vector<double>& values) {
    if (values.empty()) {
        return 0.0;
    }

    return *std::max_element(values.begin(), values.end());
}

double Statistics::CalculateMedian(const std::vector<double>& values) {
    return CalculatePercentile(values, 50.0);
}

double Statistics::CalculateVariance(const std::vector<double>& values) {
    if (values.empty()) {
        return 0.0;
    }

    double mean = CalculateMean(values);
    double sum_squared_diff = 0.0;
    for (double value : values) {
        double diff = value - mean;
        sum_squared_diff += diff * diff;
    }

    return sum_squared_diff / values.size();
}

} // namespace benchmark
} // namespace tinywebserver