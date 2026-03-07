#include "http/keep_alive_manager.h"
#include "Logger.h"
#include <algorithm>

namespace tinywebserver {

KeepAliveManager::KeepAliveManager(std::chrono::seconds default_idle_timeout)
    : default_idle_timeout_(default_idle_timeout) {
    LOG_INFO("KeepAliveManager initialized with default idle timeout: %ld seconds",
             default_idle_timeout_.count());
}

void KeepAliveManager::OnRequestStart(int fd, bool keep_alive, int idle_timeout) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto now = std::chrono::steady_clock::now();
    auto it = connections_.find(fd);

    if (it == connections_.end()) {
        // 新连接
        connections_[fd] = ConnectionState{
            .request_count = 1,
            .last_active = now,
            .keep_alive = keep_alive,
            .idle_timeout = (idle_timeout > 0) ?
                std::chrono::seconds(idle_timeout) : default_idle_timeout_
        };
        LOG_DEBUG("New connection fd=%d registered with Keep-Alive=%s, idle_timeout=%lds",
                  fd, keep_alive ? "true" : "false",
                  connections_[fd].idle_timeout.count());
    } else {
        // 现有连接，更新状态
        it->second.request_count++;
        it->second.last_active = now;
        it->second.keep_alive = keep_alive;
        if (idle_timeout > 0) {
            it->second.idle_timeout = std::chrono::seconds(idle_timeout);
        }
        LOG_DEBUG("Connection fd=%d request count=%d, Keep-Alive=%s",
                  fd, it->second.request_count, keep_alive ? "true" : "false");
    }

    total_requests_++;
}

void KeepAliveManager::OnRequestComplete(int fd) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = connections_.find(fd);
    if (it == connections_.end()) {
        LOG_WARN("OnRequestComplete called for unknown connection fd=%d", fd);
        return;
    }

    // 更新最后活动时间
    it->second.last_active = std::chrono::steady_clock::now();

    // 如果连接不是 Keep-Alive，立即标记为待关闭
    if (!it->second.keep_alive) {
        LOG_DEBUG("Connection fd=%d completed non-keep-alive request, marking for closure",
                  fd);
        // 注意：这里不立即移除，由调用者处理关闭
    }
}

void KeepAliveManager::OnConnectionClose(int fd) {
    std::lock_guard<std::mutex> lock(mutex_);

    size_t removed = connections_.erase(fd);
    if (removed > 0) {
        LOG_DEBUG("Connection fd=%d removed from KeepAliveManager", fd);
    } else {
        LOG_DEBUG("Connection fd=%d not found in KeepAliveManager", fd);
    }
}

bool KeepAliveManager::IsIdleTimeout(int fd) const {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = connections_.find(fd);
    if (it == connections_.end() || !it->second.keep_alive) {
        return false;
    }

    auto now = std::chrono::steady_clock::now();
    auto idle_duration = std::chrono::duration_cast<std::chrono::seconds>(
        now - it->second.last_active);

    return idle_duration >= it->second.idle_timeout;
}

int KeepAliveManager::GetIdleSeconds(int fd) const {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = connections_.find(fd);
    if (it == connections_.end()) {
        return -1;
    }

    auto now = std::chrono::steady_clock::now();
    auto idle_duration = std::chrono::duration_cast<std::chrono::seconds>(
        now - it->second.last_active);

    return static_cast<int>(idle_duration.count());
}

const KeepAliveManager::ConnectionState* KeepAliveManager::GetConnectionState(int fd) const {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = connections_.find(fd);
    if (it == connections_.end()) {
        return nullptr;
    }

    return &it->second;
}

std::vector<int> KeepAliveManager::CleanupTimeoutConnections() {
    std::lock_guard<std::mutex> lock(mutex_);

    std::vector<int> timed_out_fds;
    auto now = std::chrono::steady_clock::now();

    for (auto it = connections_.begin(); it != connections_.end(); ) {
        if (!it->second.keep_alive) {
            ++it;
            continue;
        }

        auto idle_duration = std::chrono::duration_cast<std::chrono::seconds>(
            now - it->second.last_active);

        if (idle_duration >= it->second.idle_timeout) {
            LOG_INFO("Connection fd=%d idle timeout (%lds > %lds), marking for closure",
                     it->first, idle_duration.count(), it->second.idle_timeout.count());
            timed_out_fds.push_back(it->first);
            it = connections_.erase(it);
            total_timeout_closures_++;
        } else {
            ++it;
        }
    }

    return timed_out_fds;
}

size_t KeepAliveManager::GetActiveConnectionCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return connections_.size();
}

int64_t KeepAliveManager::GetTotalRequestCount() const {
    return total_requests_.load(std::memory_order_relaxed);
}

void KeepAliveManager::ResetStatistics() {
    std::lock_guard<std::mutex> lock(mutex_);
    total_requests_ = 0;
    total_timeout_closures_ = 0;
}

} // namespace tinywebserver