/**
 * @file benchmark_factory.cpp
 * @brief 基准测试工厂实现
 */

#include "../../include/benchmark.h"
#include <memory>
#include <unordered_map>
#include <vector>

// 声明具体基准测试类的创建函数（在各自的cpp文件中实现）
namespace tinywebserver {
namespace benchmark {

// 前向声明创建函数
std::unique_ptr<Benchmark> CreateQpsBenchmark();
std::unique_ptr<Benchmark> CreateLatencyBenchmark();
std::unique_ptr<Benchmark> CreateMemoryBenchmark();
std::unique_ptr<Benchmark> CreateConcurrentBenchmark();

} // namespace benchmark
} // namespace tinywebserver

namespace tinywebserver {
namespace benchmark {

// 基准测试类型注册表
static const std::unordered_map<std::string, std::function<std::unique_ptr<Benchmark>()>> kBenchmarkRegistry = {
    {"qps",        CreateQpsBenchmark},
    {"latency",    CreateLatencyBenchmark},
    {"memory",     CreateMemoryBenchmark},
    {"concurrent", CreateConcurrentBenchmark}
};

std::unique_ptr<Benchmark> CreateBenchmark(const std::string& type) {
    auto it = kBenchmarkRegistry.find(type);
    if (it != kBenchmarkRegistry.end()) {
        return it->second();
    }
    return nullptr;
}

std::vector<std::string> GetAvailableBenchmarkTypes() {
    std::vector<std::string> types;
    for (const auto& entry : kBenchmarkRegistry) {
        types.push_back(entry.first);
    }
    return types;
}

// BenchmarkFactory类的方法实现（接口中定义的方法）
std::unique_ptr<Benchmark> BenchmarkFactory::Create(const std::string& benchmark_type) {
    return CreateBenchmark(benchmark_type);
}

std::vector<std::string> BenchmarkFactory::GetAvailableTypes() {
    return GetAvailableBenchmarkTypes();
}

} // namespace benchmark
} // namespace tinywebserver