#include "config/server_config.h"
#include <fstream>
#include <sstream>
#include <iostream>
#include <nlohmann/json.hpp>

namespace tinywebserver {

std::shared_ptr<ServerConfig> ServerConfig::LoadFromFile(const std::string& path) {
    auto config = std::make_shared<ServerConfig>();

    try {
        std::ifstream file(path);
        if (!file.is_open()) {
            std::cerr << "Failed to open config file: " << path << std::endl;
            return config; // 返回默认配置
        }

        nlohmann::json j;
        file >> j;
        file.close();

        if (!config->LoadFromJsonObject(j)) {
            std::cerr << "Failed to load configuration from JSON object" << std::endl;
            return config;
        }

        // 保存原始 JSON 字符串用于热重载比较
        config->original_json_ = j.dump();
    } catch (const std::exception& e) {
        std::cerr << "Error loading config file " << path << ": " << e.what() << std::endl;
    }

    return config;
}

std::shared_ptr<ServerConfig> ServerConfig::LoadFromJson(const std::string& json_str) {
    auto config = std::make_shared<ServerConfig>();

    try {
        nlohmann::json j = nlohmann::json::parse(json_str);
        if (!config->LoadFromJsonObject(j)) {
            std::cerr << "Failed to load configuration from JSON object" << std::endl;
            return config;
        }

        config->original_json_ = json_str;
    } catch (const std::exception& e) {
        std::cerr << "Error parsing JSON configuration: " << e.what() << std::endl;
    }

    return config;
}

bool ServerConfig::Reload(const std::string& new_config) {
    // 检查是否正在重载
    if (reloading_.exchange(true)) {
        std::cerr << "Configuration reload already in progress" << std::endl;
        return false;
    }

    bool success = false;
    try {
        // 加载新配置
        auto new_config_obj = LoadFromFile(new_config);
        if (!new_config_obj) {
            std::cerr << "Failed to load new configuration from " << new_config << std::endl;
            reloading_ = false;
            return false;
        }

        // 验证新配置
        auto errors = new_config_obj->Validate();
        if (!errors.empty()) {
            std::cerr << "New configuration validation failed:" << std::endl;
            for (const auto& err : errors) {
                std::cerr << "  - " << err << std::endl;
            }
            reloading_ = false;
            return false;
        }

        // 获取写入锁，更新配置
        std::unique_lock lock(mutex_);

        // 检查是否可以热重载
        if (!CanReloadWithoutRestart()) {
            std::cerr << "Configuration changes require server restart" << std::endl;
            reloading_ = false;
            return false;
        }

        // 更新配置数据
        server_ = new_config_obj->server_;
        limits_ = new_config_obj->limits_;
        logging_ = new_config_obj->logging_;
        static_ = new_config_obj->static_;
        metrics_ = new_config_obj->metrics_;

        // 更新原始 JSON
        original_json_ = new_config_obj->original_json_;

        success = true;
        std::cout << "Configuration reloaded successfully from " << new_config << std::endl;

    } catch (const std::exception& e) {
        std::cerr << "Error during configuration reload: " << e.what() << std::endl;
    }

    reloading_ = false;
    return success;
}

bool ServerConfig::CanReloadWithoutRestart() const {
    // 基础检查：limits、logging、static、metrics 可以热重载
    // server 配置（ip、port、threads、backlog）改变需要重启
    // 这个简单的实现假设调用者会在 Reload 中检查具体差异
    // 更复杂的实现可以比较新旧配置的 JSON 差异
    return true;
}

std::vector<std::string> ServerConfig::Validate() const {
    std::vector<std::string> errors;

    // server 配置验证
    if (server_.port < 1 || server_.port > 65535) {
        errors.push_back("Port must be between 1 and 65535");
    }

    if (server_.threads < 1) {
        errors.push_back("Thread count must be at least 1");
    }
    if (server_.threads > 64) {
        errors.push_back("Thread count cannot exceed 64");
    }

    if (server_.backlog < 1) {
        errors.push_back("Backlog must be at least 1");
    }
    if (server_.backlog > 65535) {
        errors.push_back("Backlog cannot exceed 65535");
    }

    if (server_.so_reuseport_sockets < 0) {
        errors.push_back("SO_REUSEPORT sockets count cannot be negative");
    }
    if (server_.so_reuseport_sockets > 64) {
        errors.push_back("SO_REUSEPORT sockets count cannot exceed 64");
    }

    // limits 配置验证
    if (limits_.max_connections < 1) {
        errors.push_back("Max connections must be at least 1");
    }
    if (limits_.max_connections > 1000000) {
        errors.push_back("Max connections cannot exceed 1,000,000");
    }

    if (limits_.max_request_size < 1024) {
        errors.push_back("Max request size must be at least 1KB");
    }
    if (limits_.max_request_size > 100 * 1024 * 1024) { // 100MB
        errors.push_back("Max request size cannot exceed 100MB");
    }

    if (limits_.max_response_size < 1024) {
        errors.push_back("Max response size must be at least 1KB");
    }
    if (limits_.max_response_size > 500 * 1024 * 1024) { // 500MB
        errors.push_back("Max response size cannot exceed 500MB");
    }

    if (limits_.connection_timeout < 1) {
        errors.push_back("Connection timeout must be at least 1 second");
    }
    if (limits_.connection_timeout > 3600) { // 1 hour
        errors.push_back("Connection timeout cannot exceed 3600 seconds");
    }

    if (limits_.keep_alive_timeout < 0) {
        errors.push_back("Keep-alive timeout cannot be negative");
    }
    if (limits_.keep_alive_timeout > 3600) {
        errors.push_back("Keep-alive timeout cannot exceed 3600 seconds");
    }

    if (limits_.max_input_buffer < 1024) {
        errors.push_back("Max input buffer must be at least 1KB");
    }
    if (limits_.max_input_buffer > 100 * 1024 * 1024) {
        errors.push_back("Max input buffer cannot exceed 100MB");
    }

    if (limits_.max_output_buffer < 1024) {
        errors.push_back("Max output buffer must be at least 1KB");
    }
    if (limits_.max_output_buffer > 500 * 1024 * 1024) {
        errors.push_back("Max output buffer cannot exceed 500MB");
    }

    // logging 配置验证
    if (logging_.queue_size < 1) {
        errors.push_back("Log queue size must be at least 1");
    }
    if (logging_.queue_size > 1000000) {
        errors.push_back("Log queue size cannot exceed 1,000,000");
    }

    if (logging_.flush_interval < 1) {
        errors.push_back("Log flush interval must be at least 1 second");
    }
    if (logging_.flush_interval > 3600) {
        errors.push_back("Log flush interval cannot exceed 3600 seconds");
    }

    // static 配置验证
    if (static_.cache_size > 10000) {
        errors.push_back("Static cache size cannot exceed 10,000");
    }

    if (static_.cache_ttl < 0) {
        errors.push_back("Static cache TTL cannot be negative");
    }
    if (static_.cache_ttl > 86400) { // 24 hours
        errors.push_back("Static cache TTL cannot exceed 86400 seconds (24 hours)");
    }

    // metrics 配置验证
    if (metrics_.prometheus_port < 1 || metrics_.prometheus_port > 65535) {
        errors.push_back("Prometheus port must be between 1 and 65535");
    }

    if (metrics_.collect_interval < 1) {
        errors.push_back("Metrics collect interval must be at least 1 second");
    }
    if (metrics_.collect_interval > 3600) {
        errors.push_back("Metrics collect interval cannot exceed 3600 seconds");
    }

    // 交叉验证
    if (metrics_.prometheus_port == server_.port) {
        errors.push_back("Prometheus port cannot be the same as server port");
    }

    return errors;
}

std::string ServerConfig::ToJsonString() const {
    try {
        nlohmann::json j;

        // server
        nlohmann::json server_json;
        server_json["ip"] = server_.ip;
        server_json["port"] = server_.port;
        server_json["threads"] = server_.threads;
        server_json["backlog"] = server_.backlog;
        server_json["tcp_nodelay"] = server_.tcp_nodelay;
        server_json["tcp_cork"] = server_.tcp_cork;
        server_json["use_so_reuseport"] = server_.use_so_reuseport;
        server_json["so_reuseport_sockets"] = server_.so_reuseport_sockets;
        j["server"] = server_json;

        // limits
        nlohmann::json limits_json;
        limits_json["max_connections"] = limits_.max_connections;
        limits_json["max_request_size"] = limits_.max_request_size;
        limits_json["max_response_size"] = limits_.max_response_size;
        limits_json["connection_timeout"] = limits_.connection_timeout;
        limits_json["keep_alive_timeout"] = limits_.keep_alive_timeout;
        limits_json["max_input_buffer"] = limits_.max_input_buffer;
        limits_json["max_output_buffer"] = limits_.max_output_buffer;
        j["limits"] = limits_json;

        // logging
        nlohmann::json logging_json;
        logging_json["level"] = logging_.level;
        logging_json["file"] = logging_.file;
        logging_json["async"] = logging_.async;
        logging_json["queue_size"] = logging_.queue_size;
        logging_json["flush_interval"] = logging_.flush_interval;
        j["logging"] = logging_json;

        // static
        nlohmann::json static_json;
        static_json["root"] = static_.root;
        static_json["cache_size"] = static_.cache_size;
        static_json["cache_ttl"] = static_.cache_ttl;
        j["static"] = static_json;

        // metrics
        nlohmann::json metrics_json;
        metrics_json["enable_prometheus"] = metrics_.enable_prometheus;
        metrics_json["prometheus_port"] = metrics_.prometheus_port;
        metrics_json["collect_interval"] = metrics_.collect_interval;
        j["metrics"] = metrics_json;

        return j.dump(2); // 缩进2个空格，便于阅读
    } catch (const std::exception& e) {
        std::cerr << "Error serializing config to JSON: " << e.what() << std::endl;
        return "{}";
    }
}

bool ServerConfig::LoadFromJsonObject(const nlohmann::json& j) {
    try {
        // 解析 server 部分
        if (j.contains("server") && j["server"].is_object()) {
            const auto& server = j["server"];
            if (server.contains("ip") && server["ip"].is_string()) {
                server_.ip = server["ip"];
            }
            if (server.contains("port") && server["port"].is_number_integer()) {
                server_.port = server["port"];
            }
            if (server.contains("threads") && server["threads"].is_number_integer()) {
                server_.threads = server["threads"];
            }
            if (server.contains("backlog") && server["backlog"].is_number_integer()) {
                server_.backlog = server["backlog"];
            }
            if (server.contains("tcp_nodelay") && server["tcp_nodelay"].is_boolean()) {
                server_.tcp_nodelay = server["tcp_nodelay"];
            }
            if (server.contains("tcp_cork") && server["tcp_cork"].is_boolean()) {
                server_.tcp_cork = server["tcp_cork"];
            }
            if (server.contains("use_so_reuseport") && server["use_so_reuseport"].is_boolean()) {
                server_.use_so_reuseport = server["use_so_reuseport"];
            }
            if (server.contains("so_reuseport_sockets") && server["so_reuseport_sockets"].is_number_integer()) {
                server_.so_reuseport_sockets = server["so_reuseport_sockets"];
            }
        }

        // 解析 limits 部分
        if (j.contains("limits") && j["limits"].is_object()) {
            const auto& limits = j["limits"];
            if (limits.contains("max_connections") && limits["max_connections"].is_number_integer()) {
                limits_.max_connections = limits["max_connections"];
            }
            if (limits.contains("max_request_size") && limits["max_request_size"].is_number_integer()) {
                limits_.max_request_size = limits["max_request_size"];
            }
            if (limits.contains("max_response_size") && limits["max_response_size"].is_number_integer()) {
                limits_.max_response_size = limits["max_response_size"];
            }
            if (limits.contains("connection_timeout") && limits["connection_timeout"].is_number_integer()) {
                limits_.connection_timeout = limits["connection_timeout"];
            }
            if (limits.contains("keep_alive_timeout") && limits["keep_alive_timeout"].is_number_integer()) {
                limits_.keep_alive_timeout = limits["keep_alive_timeout"];
            }
            if (limits.contains("max_input_buffer") && limits["max_input_buffer"].is_number_integer()) {
                limits_.max_input_buffer = limits["max_input_buffer"];
            }
            if (limits.contains("max_output_buffer") && limits["max_output_buffer"].is_number_integer()) {
                limits_.max_output_buffer = limits["max_output_buffer"];
            }
        }

        // 解析 logging 部分
        if (j.contains("logging") && j["logging"].is_object()) {
            const auto& logging = j["logging"];
            if (logging.contains("level") && logging["level"].is_string()) {
                logging_.level = logging["level"];
            }
            if (logging.contains("file") && logging["file"].is_string()) {
                logging_.file = logging["file"];
            }
            if (logging.contains("async") && logging["async"].is_boolean()) {
                logging_.async = logging["async"];
            }
            if (logging.contains("queue_size") && logging["queue_size"].is_number_integer()) {
                logging_.queue_size = logging["queue_size"];
            }
            if (logging.contains("flush_interval") && logging["flush_interval"].is_number_integer()) {
                logging_.flush_interval = logging["flush_interval"];
            }
        }

        // 解析 static 部分
        if (j.contains("static") && j["static"].is_object()) {
            const auto& static_ = j["static"];
            if (static_.contains("root") && static_["root"].is_string()) {
                this->static_.root = static_["root"];
            }
            if (static_.contains("cache_size") && static_["cache_size"].is_number_integer()) {
                this->static_.cache_size = static_["cache_size"];
            }
            if (static_.contains("cache_ttl") && static_["cache_ttl"].is_number_integer()) {
                this->static_.cache_ttl = static_["cache_ttl"];
            }
        }

        // 解析 metrics 部分
        if (j.contains("metrics") && j["metrics"].is_object()) {
            const auto& metrics = j["metrics"];
            if (metrics.contains("enable_prometheus") && metrics["enable_prometheus"].is_boolean()) {
                metrics_.enable_prometheus = metrics["enable_prometheus"];
            }
            if (metrics.contains("prometheus_port") && metrics["prometheus_port"].is_number_integer()) {
                metrics_.prometheus_port = metrics["prometheus_port"];
            }
            if (metrics.contains("collect_interval") && metrics["collect_interval"].is_number_integer()) {
                metrics_.collect_interval = metrics["collect_interval"];
            }
        }

        // 注意：metrics 部分暂不解析，由单独的指标系统处理

        return true;
    } catch (const std::exception& e) {
        std::cerr << "Failed to parse JSON configuration: " << e.what() << std::endl;
        return false;
    }
}

} // namespace tinywebserver