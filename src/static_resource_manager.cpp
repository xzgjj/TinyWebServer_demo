//


#include "static_resource_manager.h"
#include "Logger.h"
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <unistd.h>
#include <cstring>

std::shared_ptr<StaticResource> StaticResourceManager::GetResource(const std::string& path)
{
    total_requests_++;
    
    {
        // 1. 尝试读锁：查找并提升 LRU 位置
        std::unique_lock<std::shared_mutex> lock(mutex_); // 注意：调整 list 需要写锁
        auto it = cache_map_.find(path);
        if (it != cache_map_.end())
        {
            // 命中：移动到链表头部 (MRU)
            lru_list_.splice(lru_list_.begin(), lru_list_, it->second);
            cache_hits_++;
            return it->second->resource;
        }
    }

    // 2. 缓存未命中：执行加载逻辑 (需持有写锁)
    std::unique_lock<std::shared_mutex> lock(mutex_);
    
    // 双重检查
    if (cache_map_.count(path)) return cache_map_[path]->resource;

    auto resource = Load_(path);
    if (!resource) return nullptr;

    // 3. 检查并执行淘汰策略
    while (current_cache_size_ + resource->size > max_cache_size_ && !lru_list_.empty())
    {
        Evict_();
    }

    // 4. 插入新资源
    lru_list_.push_front({path, resource});
    cache_map_[path] = lru_list_.begin();
    current_cache_size_ += resource->size;

    return resource;
}

std::shared_ptr<StaticResource> StaticResourceManager::Load_(const std::string& path)
{
    int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0)
    {
        LOG_WARN("StaticResource: Open failed %s", path.c_str());
        return nullptr;
    }

    struct stat st;
    if (::fstat(fd, &st) < 0 || st.st_size == 0)
    {
        ::close(fd);
        return nullptr;
    }

    void* addr = ::mmap(nullptr, st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    ::close(fd);

    if (addr == MAP_FAILED)
    {
        LOG_ERROR("StaticResource: Mmap failed %s", path.c_str());
        return nullptr;
    }

    // 使用自定义删除器确保 munmap
    size_t size = static_cast<size_t>(st.st_size);
    auto res = new StaticResource{addr, size, path};
    
    return std::shared_ptr<StaticResource>(res, [this, size](StaticResource* p) {
        if (p)
        {
            LOG_DEBUG("StaticResource: munmap addr=%p, size=%zu, path=%s", p->addr, p->size, p->path.c_str());
            ::munmap(p->addr, p->size);
            // 注意：此处不减 current_cache_size_，因为该变量追踪的是缓存管理池的大小
            // 当资源从 lru_list 移除时才减少
            delete p;
        }
    });
}

void StaticResourceManager::Evict_()
{
    if (lru_list_.empty()) return;

    // 淘汰末尾 (最久未访问)
    auto last = lru_list_.back();
    LOG_INFO("StaticResource: Evicting cache item: %s, size: %zu", last.path.c_str(), last.resource->size);
    
    current_cache_size_ -= last.resource->size;
    cache_map_.erase(last.path);
    lru_list_.pop_back();
    // last.resource 离开作用域，引用计数减1。若无连接在使用，则触发 munmap。
}

StaticResourceManager::Status StaticResourceManager::GetStatus() const
{
    std::shared_lock<std::shared_mutex> lock(mutex_);
    return {
        current_cache_size_.load(),
        cache_map_.size(),
        total_requests_.load(),
        cache_hits_.load()
    };
}