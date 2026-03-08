#include "plugin/example_plugin.h"
#include "server.h"
#include "Logger.h"
#include "http_request.h"
#include "http_response.h"
#include <sstream>

bool ExamplePlugin::OnLoad(Server& server) {
    LOG_INFO("[ExamplePlugin] Plugin loaded successfully");
    connection_count_ = 0;
    request_count_ = 0;
    return true;
}

bool ExamplePlugin::OnUnload() {
    LOG_INFO("[ExamplePlugin] Plugin unloaded. Statistics: connections=%d, requests=%d",
             connection_count_, request_count_);
    return true;
}

void ExamplePlugin::OnRequestStart(HttpRequest& request) {
    request_count_++;

    // 示例：添加自定义请求头部（如果不存在）
    // 注意：这里只是演示，实际插件可能修改请求

    std::string method = request.GetMethod();
    std::string path = request.GetPath();

    LOG_DEBUG("[ExamplePlugin] Request start: %s %s (total requests: %d)",
              method.c_str(), path.c_str(), request_count_);
}

void ExamplePlugin::OnRequestComplete(HttpRequest& request, HttpResponse& response) {
    // 示例：添加自定义响应头部
    // 注意：HttpResponse 需要提供添加头部的方法
    // 目前 HttpResponse 没有公开的添加头部方法，这里只是演示

    int status_code = response.GetCode();
    LOG_DEBUG("[ExamplePlugin] Request complete: %s %s -> %d",
              request.GetMethod().c_str(), request.GetPath().c_str(), status_code);
}

void ExamplePlugin::OnConnectionOpen(int fd) {
    connection_count_++;
    LOG_DEBUG("[ExamplePlugin] Connection opened: fd=%d (total connections: %d)",
              fd, connection_count_);
}

void ExamplePlugin::OnConnectionClose(int fd) {
    LOG_DEBUG("[ExamplePlugin] Connection closed: fd=%d", fd);
    // 注意：这里不减少 connection_count_，因为连接关闭时我们不知道当前活跃连接数
    // 实际插件可能需要更精确的连接跟踪
}