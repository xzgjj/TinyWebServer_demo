#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <vector>

// 包含 JSON 库头文件
#include "json.hpp"

namespace tinywebserver {

/**
 * @brief 服务器配置类
 *
 * 支持从 JSON 文件加载配置，提供热重载能力。
 * 线程安全，支持多线程并发读取配置。
 */
class ServerConfig {
public:
    // 服务器配置结构
    struct ServerOptions {
        std::string ip = "0.0.0.0";
        int port = 8080;
        int threads = 4;
        int backlog = 1024;
        bool tcp_nodelay = true;
        bool tcp_cork = false;
    };

    // 资源限制配置
    struct LimitsOptions {
        int max_connections = 10000;
        size_t max_request_size = 65536;          // 64KB
        size_t max_response_size = 1048576;       // 1MB
        int connection_timeout = 30;              // 秒
        int keep_alive_timeout = 15;              // 秒
        size_t max_input_buffer = 65536;          // 64KB
        size_t max_output_buffer = 1048576;       // 1MB
    };

    // 日志配置
    struct LoggingOptions {
        std::string level = "INFO";
        std::string file = "logs/server.log";
        bool async = true;
        size_t queue_size = 10000;
        int flush_interval = 3;                   // 秒
    };

    // 静态资源配置
    struct StaticOptions {
        std::string root = "./www";
        size_t cache_size = 100;
        int cache_ttl = 300;                      // 秒
    };

    // 监控指标配置
    struct MetricsOptions {
        bool enable_prometheus = true;
        int prometheus_port = 9090;
        int collect_interval = 5;                   // 秒
    };

    /**
     * @brief 从文件加载配置
     * @param path 配置文件路径
     * @return 配置对象
     */
    static std::shared_ptr<ServerConfig> LoadFromFile(const std::string& path);

    /**
     * @brief 从 JSON 字符串加载配置
     * @param json_str JSON 字符串
     * @return 配置对象
     */
    static std::shared_ptr<ServerConfig> LoadFromJson(const std::string& json_str);

    /**
     * @brief 获取服务器配置（只读）
     */
    ServerOptions GetServerOptions() const {
        std::shared_lock lock(mutex_);
        return server_;
    }

    /**
     * @brief 获取资源限制配置（只读）
     */
    LimitsOptions GetLimitsOptions() const {
        std::shared_lock lock(mutex_);
        return limits_;
    }

    /**
     * @brief 获取日志配置（只读）
     */
    LoggingOptions GetLoggingOptions() const {
        std::shared_lock lock(mutex_);
        return logging_;
    }

    /**
     * @brief 获取静态资源配置（只读）
     */
    StaticOptions GetStaticOptions() const {
        std::shared_lock lock(mutex_);
        return static_;
    }

    /**
     * @brief 获取监控指标配置（只读）
     */
    MetricsOptions GetMetricsOptions() const {
        std::shared_lock lock(mutex_);
        return metrics_;
    }

    /**
     * @brief 热重载配置
     * @param new_config 新的配置文件路径
     * @return 是否重载成功
     */
    bool Reload(const std::string& new_config);

    /**
     * @brief 检查是否可以无需重启热重载
     */
    bool CanReloadWithoutRestart() const;

    /**
     * @brief 验证配置的合法性
     * @return 错误消息列表，空表示验证通过
     */
    std::vector<std::string> Validate() const;

    /**
     * @brief 转换为 JSON 字符串（用于调试）
     */
    std::string ToJsonString() const;

    /**
     * @brief 默认构造函数（用于创建默认配置）
     */
    ServerConfig() = default;

private:

    // 从 nlohmann::json 对象加载配置
    bool LoadFromJsonObject(const nlohmann::json& j);

    // 配置数据
    ServerOptions server_;
    LimitsOptions limits_;
    LoggingOptions logging_;
    StaticOptions static_;
    MetricsOptions metrics_;

    // 原始 JSON 数据（用于热重载比较）
    std::string original_json_;

    // 线程安全保护
    mutable std::shared_mutex mutex_;

    // 原子标志，表示正在重载
    std::atomic<bool> reloading_{false};
};

} // namespace tinywebserver