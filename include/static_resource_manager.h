//模块负责管理 mmap 的生命周期，确保高并发下的线程安全和内存安全

#ifndef STATIC_RESOURCE_MANAGER_H
#define STATIC_RESOURCE_MANAGER_H

#include <string>
#include <unordered_map>
#include <shared_mutex>
#include <memory>
#include <list>
#include <atomic>
#include <mutex>

/**
 * @brief 封装 mmap 映射的资源块
 */
struct StaticResource
{
    void* addr = nullptr;
    size_t size = 0;
    std::string path; 
};

/**
 * @brief 静态资源管理器 (单例)
 * 阶段四特性：LRU 淘汰策略、内存上限控制、运行时统计
 */
class StaticResourceManager
{
public:
    static StaticResourceManager& GetInstance()
    {
        static StaticResourceManager instance;
        return instance;
    }

    StaticResourceManager(const StaticResourceManager&) = delete;
    StaticResourceManager& operator=(const StaticResourceManager&) = delete;

    /**
     * @brief 获取静态资源 (带 LRU 更新)
     */
    std::shared_ptr<StaticResource> GetResource(const std::string& path);

    /**
     * @brief 设置缓存限制
     * @param max_mem_bytes 最大允许 mmap 的字节数
     */
    void SetCacheLimit(size_t max_mem_bytes) 
    { 
        max_cache_size_ = max_mem_bytes; 
    }

    /**
     * @brief 运行时状态查询接口 (生产级要求)
     */
    struct Status
    {
        size_t current_memory_usage;
        size_t cached_files_count;
        uint64_t total_requests;
        uint64_t cache_hits;
    };
    Status GetStatus() const;

private:
    StaticResourceManager() : max_cache_size_(1024 * 1024 * 512) {} // 默认 512MB
    ~StaticResourceManager() = default;

    std::shared_ptr<StaticResource> Load_(const std::string& path);
    void Evict_(); // 执行 LRU 淘汰

    // 结构定义：缓存项
    struct CacheItem
    {
        std::string path;
        std::shared_ptr<StaticResource> resource;
    };

    mutable std::shared_mutex mutex_;
    size_t max_cache_size_;
    std::atomic<size_t> current_cache_size_{0};
    
    // LRU 数据结构
    std::list<CacheItem> lru_list_;
    std::unordered_map<std::string, std::list<CacheItem>::iterator> cache_map_;

    // 统计数据
    std::atomic<uint64_t> total_requests_{0};
    std::atomic<uint64_t> cache_hits_{0};
};

#endif

