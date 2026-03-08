#include "plugin/plugin_manager.h"
#include "server.h"
#include "http_request.h"
#include "http_response.h"
#include <algorithm>

PluginManager& PluginManager::GetInstance() {
    static PluginManager instance;
    return instance;
}

size_t PluginManager::LoadAllPlugins(Server& server) {
    std::lock_guard<std::mutex> lock(mutex_);

    size_t loaded_count = 0;
    for (auto& pair : plugins_) {
        if (pair.second->OnLoad(server)) {
            loaded_count++;
        } else {
            // 加载失败，记录日志（TODO：添加日志）
        }
    }

    plugins_loaded_ = true;
    return loaded_count;
}

size_t PluginManager::UnloadAllPlugins() {
    std::lock_guard<std::mutex> lock(mutex_);

    size_t unloaded_count = 0;
    for (auto& pair : plugins_) {
        if (pair.second->OnUnload()) {
            unloaded_count++;
        } else {
            // 卸载失败，记录日志（TODO：添加日志）
        }
    }

    plugins_loaded_ = false;
    return unloaded_count;
}

Plugin* PluginManager::GetPlugin(const std::string& name) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = plugins_.find(name);
    return it != plugins_.end() ? it->second.get() : nullptr;
}

std::vector<std::string> PluginManager::GetPluginNames() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::string> names;
    names.reserve(plugins_.size());

    for (const auto& pair : plugins_) {
        names.push_back(pair.first);
    }

    return names;
}

bool PluginManager::IsPluginLoaded(const std::string& name) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = plugins_.find(name);
    if (it == plugins_.end()) {
        return false;
    }
    // TODO: 实际应该检查插件是否真的已加载（通过状态标志）
    return plugins_loaded_;
}

void PluginManager::NotifyRequestStart(HttpRequest& request) {
    std::lock_guard<std::mutex> lock(mutex_);

    for (auto& pair : plugins_) {
        try {
            pair.second->OnRequestStart(request);
        } catch (const std::exception& e) {
            // 插件回调异常，记录日志但不中断其他插件（TODO：添加日志）
            (void)e;
        }
    }
}

void PluginManager::NotifyRequestComplete(HttpRequest& request, HttpResponse& response) {
    std::lock_guard<std::mutex> lock(mutex_);

    for (auto& pair : plugins_) {
        try {
            pair.second->OnRequestComplete(request, response);
        } catch (const std::exception& e) {
            // 插件回调异常，记录日志但不中断其他插件（TODO：添加日志）
            (void)e;
        }
    }
}

void PluginManager::NotifyConnectionOpen(int fd) {
    std::lock_guard<std::mutex> lock(mutex_);

    for (auto& pair : plugins_) {
        try {
            pair.second->OnConnectionOpen(fd);
        } catch (const std::exception& e) {
            // 插件回调异常，记录日志但不中断其他插件（TODO：添加日志）
            (void)e;
        }
    }
}

void PluginManager::NotifyConnectionClose(int fd) {
    std::lock_guard<std::mutex> lock(mutex_);

    for (auto& pair : plugins_) {
        try {
            pair.second->OnConnectionClose(fd);
        } catch (const std::exception& e) {
            // 插件回调异常，记录日志但不中断其他插件（TODO：添加日志）
            (void)e;
        }
    }
}