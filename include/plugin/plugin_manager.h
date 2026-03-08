#pragma once

#include <memory>
#include <vector>
#include <unordered_map>
#include <string>
#include <mutex>
#include "plugin/plugin.h"

class Server;

/**
 * @brief 插件管理器
 *
 * 负责插件的生命周期管理和事件分发，支持：
 * - 静态插件注册
 * - 动态库插件加载（TODO）
 * - 插件依赖管理（TODO）
 * - 事件通知链
 */
class PluginManager {
public:
    /**
     * @brief 获取插件管理器单例实例
     */
    static PluginManager& GetInstance();

    // 禁用拷贝和移动
    PluginManager(const PluginManager&) = delete;
    PluginManager& operator=(const PluginManager&) = delete;
    PluginManager(PluginManager&&) = delete;
    PluginManager& operator=(PluginManager&&) = delete;

    /**
     * @brief 注册静态插件
     * @tparam T 插件类型，必须继承自 Plugin
     * @return true 表示注册成功，false 表示同名插件已存在
     *
     * @note 静态插件在程序启动时注册，无需动态加载
     * @example PluginManager::GetInstance().RegisterPlugin<MyPlugin>();
     */
    template<typename T>
    bool RegisterPlugin() {
        static_assert(std::is_base_of<Plugin, T>::value,
                      "T must be derived from Plugin");

        auto plugin = std::make_unique<T>();
        std::string name = plugin->Name();

        std::lock_guard<std::mutex> lock(mutex_);
        if (plugins_.find(name) != plugins_.end()) {
            return false; // 同名插件已存在
        }

        plugins_[name] = std::move(plugin);
        return true;
    }

    /**
     * @brief 加载所有已注册的插件
     * @param server 服务器实例
     * @return 成功加载的插件数量
     */
    size_t LoadAllPlugins(Server& server);

    /**
     * @brief 卸载所有插件
     * @return 成功卸载的插件数量
     */
    size_t UnloadAllPlugins();

    /**
     * @brief 获取指定插件
     * @param name 插件名称
     * @return 插件指针，如果不存在则返回 nullptr
     */
    Plugin* GetPlugin(const std::string& name) const;

    /**
     * @brief 获取所有插件名称
     * @return 插件名称列表
     */
    std::vector<std::string> GetPluginNames() const;

    /**
     * @brief 获取插件数量
     */
    size_t GetPluginCount() const { return plugins_.size(); }

    /**
     * @brief 检查插件是否已加载
     * @param name 插件名称
     */
    bool IsPluginLoaded(const std::string& name) const;

    /**
     * @brief 事件通知：请求开始
     */
    void NotifyRequestStart(HttpRequest& request);

    /**
     * @brief 事件通知：请求完成
     */
    void NotifyRequestComplete(HttpRequest& request, HttpResponse& response);

    /**
     * @brief 事件通知：连接建立
     */
    void NotifyConnectionOpen(int fd);

    /**
     * @brief 事件通知：连接关闭
     */
    void NotifyConnectionClose(int fd);

    /**
     * @brief 动态库插件加载（占位符，TODO）
     * @param path 动态库路径
     * @return true 表示加载成功
     */
    bool LoadDynamicPlugin(const std::string& path) {
        // TODO: 实现动态库加载
        (void)path;
        return false;
    }

    /**
     * @brief 动态库插件卸载（占位符，TODO）
     * @param name 插件名称
     * @return true 表示卸载成功
     */
    bool UnloadDynamicPlugin(const std::string& name) {
        // TODO: 实现动态库卸载
        (void)name;
        return false;
    }

private:
    PluginManager() = default;
    ~PluginManager() = default;

    mutable std::mutex mutex_;
    std::unordered_map<std::string, std::unique_ptr<Plugin>> plugins_;
    bool plugins_loaded_ = false;
};