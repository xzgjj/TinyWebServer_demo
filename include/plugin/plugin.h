#pragma once

#include <string>
#include <memory>
#include <functional>
#include "http_request.h"
#include "http_response.h"

// 前向声明
class Server;

/**
 * @brief 插件接口
 *
 * 插件系统允许通过动态模块扩展服务器功能，支持以下事件：
 * - 插件加载/卸载
 * - 请求处理前后回调
 * - 连接建立/关闭回调
 */
class Plugin {
public:
    virtual ~Plugin() = default;

    /**
     * @brief 获取插件名称
     * @return 插件名称，必须唯一
     */
    virtual std::string Name() const = 0;

    /**
     * @brief 获取插件版本
     * @return 版本字符串，格式建议为 "major.minor.patch"
     */
    virtual std::string Version() const = 0;

    /**
     * @brief 插件加载时调用
     * @param server 服务器实例引用
     * @return true 表示加载成功，false 表示加载失败
     */
    virtual bool OnLoad(Server& server) = 0;

    /**
     * @brief 插件卸载时调用
     * @return true 表示卸载成功，false 表示卸载失败
     */
    virtual bool OnUnload() = 0;

    /**
     * @brief 请求开始处理时调用
     * @param request HTTP 请求对象
     * @note 可以修改请求对象，添加自定义头部等
     */
    virtual void OnRequestStart([[maybe_unused]] HttpRequest& request) {}

    /**
     * @brief 请求处理完成时调用
     * @param request HTTP 请求对象
     * @param response HTTP 响应对象
     * @note 可以修改响应对象，添加自定义头部或修改内容
     */
    virtual void OnRequestComplete([[maybe_unused]] HttpRequest& request, [[maybe_unused]] HttpResponse& response) {}

    /**
     * @brief 新连接建立时调用
     * @param fd 连接的文件描述符
     */
    virtual void OnConnectionOpen([[maybe_unused]] int fd) {}

    /**
     * @brief 连接关闭时调用
     * @param fd 连接的文件描述符
     */
    virtual void OnConnectionClose([[maybe_unused]] int fd) {}
};

/**
 * @brief 插件创建函数类型
 * 动态库插件必须导出此函数
 */
using PluginCreateFunc = std::function<std::unique_ptr<Plugin>()>;

/**
 * @brief 插件销毁函数类型
 * 动态库插件必须导出此函数
 */
using PluginDestroyFunc = std::function<void(Plugin*)>;