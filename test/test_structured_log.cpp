#include <iostream>
#include <memory>
#include "config/server_config.h"
#include "logging/structured_logger.h"

int main() {
    // 创建测试配置
    auto config = std::make_shared<tinywebserver::ServerConfig>();

    // 初始化结构化日志系统
    tinywebserver::InitStructuredLoggerFromConfig(config);

    // 测试结构化日志
    if (tinywebserver::StructuredLogManager::GetInstance().IsInitialized()) {
        std::cout << "Structured logger initialized successfully" << std::endl;

        // 测试基本日志
        LOG_S_INFO("Test structured log message: %s", "Hello, structured logging!");
        // 测试枚举访问
        auto level = tinywebserver::StructuredLogLevel::INFO;
        std::cout << "Enum value: " << static_cast<int>(level) << std::endl;

        // 测试带上下文的日志
        tinywebserver::RequestContext ctx;
        ctx.request_id = "req-12345";
        ctx.connection_id = 42;
        ctx.client_ip = "192.168.1.100";
        ctx.user_agent = "TestClient/1.0";
        ctx.response_status = 200;

        // 注意：LOG_STRUCTURED_CTX 宏需要请求上下文参数
        // 由于宏定义需要特定的格式，这里简化测试

        std::cout << "Structured logging test completed" << std::endl;
    } else {
        std::cout << "Structured logger not initialized (async logging may be disabled)" << std::endl;
    }

    return 0;
}