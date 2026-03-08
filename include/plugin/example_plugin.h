#pragma once

#include "plugin/plugin.h"
#include <string>

/**
 * @brief 示例插件：记录连接和请求事件
 *
 * 这是一个演示插件，展示如何实现 Plugin 接口。
 * 功能：
 * - 记录连接建立和关闭事件
 * - 记录请求开始和完成事件
 * - 添加自定义请求头部
 * - 添加自定义响应头部
 */
class ExamplePlugin : public Plugin {
public:
    ExamplePlugin() = default;
    ~ExamplePlugin() override = default;

    // Plugin 接口实现
    std::string Name() const override { return "ExamplePlugin"; }
    std::string Version() const override { return "1.0.0"; }

    bool OnLoad(Server& server) override;
    bool OnUnload() override;

    void OnRequestStart(HttpRequest& request) override;
    void OnRequestComplete(HttpRequest& request, HttpResponse& response) override;
    void OnConnectionOpen(int fd) override;
    void OnConnectionClose(int fd) override;

private:
    int connection_count_ = 0;
    int request_count_ = 0;
};