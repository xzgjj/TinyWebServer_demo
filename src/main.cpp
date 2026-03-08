//
#include <iostream>
#include <csignal>
#include <memory>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <fstream>
#include "server.h"
#include "http_response.h"
#include "http_request.h"
#include "connection.h"
#include "Logger.h"
#include "request_validator.h"
#include "config/server_config.h"
#include "logging/structured_logger.h"
#include "plugin/plugin_manager.h"
#include "plugin/example_plugin.h"

int main(int argc, char* argv[]) {
    std::string config_file;
    // 简单命令行参数解析
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--config" && i + 1 < argc) {
            config_file = argv[++i];
        } else if (arg == "--help") {
            std::cout << "Usage: " << argv[0] << " [--config <path>] [--help]" << std::endl;
            return 0;
        } else {
            std::cerr << "Unknown argument: " << arg << std::endl;
            return 1;
        }
    }

    // 1. 确保环境收敛：创建资源目录（构建时已创建，此处作为后备）
    struct stat st;
    if (stat("./public", &st) != 0) {
        mkdir("./public", 0755);
    }
    // 确保默认首页存在（构建时已创建，此处作为后备）
    if (stat("./public/index.html", &st) != 0) {
        // 使用C++文件操作替代system调用
        std::ofstream index_file("./public/index.html");
        if (index_file.is_open()) {
            index_file << "<h1>TinyWebServer V3</h1>";
            index_file.close();
        }
    }

    // 2. 确保日志目录存在（构建时已创建，此处作为后备）
    if (stat("./local", &st) != 0) {
        mkdir("./local", 0755);
    }
    if (stat("./local/logs", &st) != 0) {
        mkdir("./local/logs", 0755);
    }
    if (stat("./local/logs/benchmark", &st) != 0) {
        mkdir("./local/logs/benchmark", 0755);
    }

    Logger::GetInstance().Init("./local/logs/tiny_server.log", LogLevel::LOG_LEVEL_INFO);

    std::unique_ptr<Server> server;
    std::shared_ptr<tinywebserver::ServerConfig> config = nullptr;

    if (!config_file.empty()) {
        config = tinywebserver::ServerConfig::LoadFromFile(config_file);
        if (!config) {
            std::cerr << "Failed to load configuration from " << config_file << std::endl;
            return 1;
        }
        server = std::make_unique<Server>(config, PluginManager::GetInstance());
        LOG_INFO("Server configured from file: %s", config_file.c_str());

        // 初始化结构化日志系统
        tinywebserver::InitStructuredLoggerFromConfig(config);
    } else {
        // 默认配置
        server = std::make_unique<Server>("0.0.0.0", 8080, PluginManager::GetInstance());
        // 创建默认配置对象以获取默认值
        config = std::make_shared<tinywebserver::ServerConfig>();
    }

    // 确定静态资源根目录和 Keep-Alive 超时
    std::string static_root = "./public"; // 默认值
    int keep_alive_timeout = 15; // 默认 15 秒
    if (config) {
        static_root = config->GetStaticOptions().root;
        keep_alive_timeout = config->GetLimitsOptions().keep_alive_timeout;
    }

    // 注册并加载插件
    PluginManager& plugin_manager = PluginManager::GetInstance();
    plugin_manager.RegisterPlugin<ExamplePlugin>();
    server->LoadPlugins();

    auto* server_ptr = server.get();
    server->SetOnMessage([static_root, keep_alive_timeout, server_ptr](std::shared_ptr<Connection> conn, const std::string& /*data*/) {
        auto parser = conn->GetHttpParser();
        auto& buffer = conn->GetInputBuffer();

        // 核心修复：循环处理，直到缓冲区数据不足以构成一个完整 Header
        while (true) {
            size_t header_end = buffer.find("\r\n\r\n");
            if (header_end == std::string::npos) {
                break; // 数据不足，跳出等待下次 Read
            }

            if (parser->Parse(buffer)) {
                // Keep-Alive 管理：通知连接开始处理请求
                conn->OnRequestStart(parser->IsKeepAlive(), keep_alive_timeout);

                // 插件事件：请求开始
                server_ptr->GetPluginManager().NotifyRequestStart(*parser);

                // 请求安全验证
                tinywebserver::RequestValidator validator(static_root);
                auto validation_result = validator.ValidateRequest(*parser);

                HttpResponse response;
                if (!validation_result.valid) {
                    // 验证失败，生成错误响应
                    // 根据错误类型选择状态码
                    int error_code = 400; // Bad Request
                    if (validation_result.error.GetCode() == tinywebserver::WebError::kInvalidPath) {
                        error_code = 403; // Forbidden 或 404 Not Found
                    } else if (validation_result.error.GetCode() == tinywebserver::WebError::kRequestTooLarge) {
                        error_code = 413; // Payload Too Large
                    } else if (validation_result.error.GetCode() == tinywebserver::WebError::kUnsupportedMethod) {
                        error_code = 405; // Method Not Allowed
                    }

                    // 生成错误响应，使用错误码
                    response.Init(static_root, "", parser->IsKeepAlive(), error_code, parser.get());
                    response.MakeResponse();

                    LOG_WARN("Request validation failed: %s (code: %d)",
                             validation_result.error.ToString().c_str(), error_code);
                } else {
                    // 验证成功，使用规范化路径
                    // 确保路径以斜杠开头，以便正确拼接
                    std::string normalized_path = validation_result.normalized_path;
                    if (!normalized_path.empty() && normalized_path[0] != '/') {
                        normalized_path = "/" + normalized_path;
                    }
                    response.Init(static_root, normalized_path, parser->IsKeepAlive(), -1, parser.get());
                    response.MakeResponse();
                }

                // 插件事件：请求完成
                server_ptr->GetPluginManager().NotifyRequestComplete(*parser, response);

                // 异步发送：Reactor 会处理发送队列
                conn->Send(response.GetHeaderString());
                if (response.HasFileBody()) {
                    conn->Send(response.GetFileBody());
                } else {
                    conn->Send(response.GetBodyString());
                }

                // --- 关键：精确消耗已解析的数据 ---
                // 注意：这里假设 Parse 仅处理了 Header，Body 逻辑需视业务而定
                // 在当前简单模型下，我们移除到 \r\n\r\n 之后
                buffer.erase(0, header_end + 4);

                int status_code = response.GetCode();
                parser->Reset(); // 为下一次解析重置状态
                conn->OnRequestComplete(); // Keep-Alive 管理：请求处理完成

                // 如果出错，则优雅关闭写端
                if (status_code >= 400) {
                    conn->Shutdown();
                    break;
                }
            } else {
                // 解析协议错误，清除坏数据并断开
                buffer.clear();
                conn->Shutdown();
                break;
            }
        }
    });

    LOG_INFO("Server starting on port 8080...");
    server->Start();
    server->Run();
    return 0;
}
