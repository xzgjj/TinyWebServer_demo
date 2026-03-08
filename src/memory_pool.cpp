#include "memory_pool.h"
#include "Logger.h"
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <stdexcept>

namespace tinywebserver {

// MemoryPool::Slab 实现
MemoryPool::Slab::Slab(size_t block_size, size_t block_count)
    : block_size(block_size), block_count(block_count), free_count(block_count) {
    // 分配对齐的内存块
    size_t total_size = block_size * block_count;
    memory = std::aligned_alloc(alignof(std::max_align_t), total_size);
    if (!memory) {
        LOG_ERROR("MemoryPool::Slab: Failed to allocate %zu bytes", total_size);
        throw std::bad_alloc();
    }

    // 初始化使用状态
    used.resize(block_count, false);

    LOG_DEBUG("MemoryPool::Slab created: block_size=%zu, block_count=%zu, total=%zu",
              block_size, block_count, total_size);
}

MemoryPool::Slab::~Slab() {
    if (memory) {
        std::free(memory);
        memory = nullptr;
    }
}

void* MemoryPool::Slab::AllocateBlock() {
    if (free_count == 0) {
        return nullptr; // Slab已满
    }

    // 查找第一个空闲块
    for (size_t i = 0; i < block_count; ++i) {
        if (!used[i]) {
            used[i] = true;
            free_count--;
            void* ptr = static_cast<char*>(memory) + i * block_size;

            // 清零内存（可选，安全起见）
            std::memset(ptr, 0, block_size);

            LOG_DEBUG("MemoryPool::Slab::AllocateBlock: allocated block %zu/%zu at %p",
                      i, block_count, ptr);
            return ptr;
        }
    }

    return nullptr; // 不应该执行到这里
}

bool MemoryPool::Slab::DeallocateBlock(void* ptr) {
    if (!Contains(ptr)) {
        return false;
    }

    // 计算块索引
    auto offset = static_cast<char*>(ptr) - static_cast<char*>(memory);
    size_t index = offset / block_size;

    if (index >= block_count || !used[index]) {
        LOG_ERROR("MemoryPool::Slab::DeallocateBlock: invalid block at %p", ptr);
        return false;
    }

    used[index] = false;
    free_count++;

    LOG_DEBUG("MemoryPool::Slab::DeallocateBlock: freed block %zu/%zu at %p",
              index, block_count, ptr);
    return true;
}

bool MemoryPool::Slab::Contains(void* ptr) const {
    if (!memory) return false;

    auto p = static_cast<char*>(ptr);
    auto start = static_cast<char*>(memory);
    auto end = start + block_size * block_count;

    return p >= start && p < end;
}

// MemoryPool 实现
MemoryPool::MemoryPool() {
    stats_ = {0, 0, 0, 0, 0, 0};
    LOG_INFO("MemoryPool initialized");
}

MemoryPool::~MemoryPool() {
    Clear();
    LOG_INFO("MemoryPool destroyed");
}

MemoryPool& MemoryPool::GetInstance() {
    static MemoryPool instance;
    return instance;
}

MemoryPool::SizeClass MemoryPool::GetSizeClass(size_t size) const {
    if (size <= static_cast<size_t>(SizeClass::SMALL)) {
        return SizeClass::SMALL;
    } else if (size <= static_cast<size_t>(SizeClass::MEDIUM)) {
        return SizeClass::MEDIUM;
    } else if (size <= static_cast<size_t>(SizeClass::LARGE)) {
        return SizeClass::LARGE;
    } else {
        return SizeClass::HUGE;
    }
}

MemoryPool::Slab* MemoryPool::CreateSlab(SizeClass size_class) {
    size_t block_size = 0;
    size_t block_count = 0;

    // 根据大小类别配置Slab参数
    switch (size_class) {
        case SizeClass::SMALL:
            block_size = static_cast<size_t>(SizeClass::SMALL);
            block_count = 128; // 每个Slab 128个小块
            break;
        case SizeClass::MEDIUM:
            block_size = static_cast<size_t>(SizeClass::MEDIUM);
            block_count = 64; // 每个Slab 64个中块
            break;
        case SizeClass::LARGE:
            block_size = static_cast<size_t>(SizeClass::LARGE);
            block_count = 16; // 每个Slab 16个大块
            break;
        case SizeClass::HUGE:
            // HUGE类别不使用Slab
            return nullptr;
    }

    try {
        auto slab = std::make_unique<Slab>(block_size, block_count);
        auto slab_ptr = slab.get();
        slabs_[size_class].push_back(std::move(slab));

        stats_.slab_count++;
        stats_.free_blocks += block_count;

        LOG_DEBUG("MemoryPool::CreateSlab: created slab for size_class=%zu, block_size=%zu, block_count=%zu",
                  static_cast<size_t>(size_class), block_size, block_count);

        return slab_ptr;
    } catch (const std::bad_alloc& e) {
        LOG_ERROR("MemoryPool::CreateSlab: failed to create slab: %s", e.what());
        return nullptr;
    }
}

MemoryPool::Slab* MemoryPool::FindSlabContaining(void* ptr) {
    // 检查所有Slab
    for (auto& [size_class, slab_list] : slabs_) {
        for (auto& slab : slab_list) {
            if (slab->Contains(ptr)) {
                return slab.get();
            }
        }
    }
    return nullptr;
}

void* MemoryPool::Allocate(size_t size) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (size == 0) {
        return nullptr;
    }

    SizeClass size_class = GetSizeClass(size);

    // HUGE大小直接使用系统分配器
    if (size_class == SizeClass::HUGE) {
        void* ptr = std::malloc(size);
        if (ptr) {
            stats_.total_allocated_bytes += size;
            stats_.current_used_bytes += size;
            stats_.allocated_blocks++;

            LOG_DEBUG("MemoryPool::Allocate: huge allocation %zu bytes at %p", size, ptr);
        } else {
            LOG_ERROR("MemoryPool::Allocate: malloc failed for %zu bytes", size);
        }
        return ptr;
    }

    // 查找有空闲块的Slab
    auto& slab_list = slabs_[size_class];
    for (auto& slab : slab_list) {
        if (slab->free_count > 0) {
            void* ptr = slab->AllocateBlock();
            if (ptr) {
                stats_.total_allocated_bytes += size;
                stats_.current_used_bytes += size;
                stats_.allocated_blocks++;
                stats_.free_blocks--;

                LOG_DEBUG("MemoryPool::Allocate: allocated %zu bytes (class=%zu) at %p",
                          size, static_cast<size_t>(size_class), ptr);
                return ptr;
            }
        }
    }

    // 没有可用Slab，创建新的
    Slab* new_slab = CreateSlab(size_class);
    if (!new_slab) {
        LOG_ERROR("MemoryPool::Allocate: failed to create slab for size %zu", size);
        return nullptr;
    }

    void* ptr = new_slab->AllocateBlock();
    if (ptr) {
        stats_.total_allocated_bytes += size;
        stats_.current_used_bytes += size;
        stats_.allocated_blocks++;
        stats_.free_blocks--;

        LOG_DEBUG("MemoryPool::Allocate: allocated from new slab %zu bytes at %p", size, ptr);
    }

    return ptr;
}

void MemoryPool::Deallocate(void* ptr, size_t size) {
    if (!ptr) return;

    std::lock_guard<std::mutex> lock(mutex_);

    SizeClass size_class = GetSizeClass(size);

    // HUGE大小直接使用系统释放
    if (size_class == SizeClass::HUGE) {
        std::free(ptr);
        stats_.total_freed_bytes += size;
        stats_.current_used_bytes -= size;
        stats_.allocated_blocks--;

        LOG_DEBUG("MemoryPool::Deallocate: freed huge block %zu bytes at %p", size, ptr);
        return;
    }

    // 查找包含该指针的Slab
    Slab* slab = FindSlabContaining(ptr);
    if (!slab) {
        LOG_ERROR("MemoryPool::Deallocate: cannot find slab containing %p", ptr);
        // 作为安全措施，尝试直接释放（尽管不应该发生）
        std::free(ptr);
        return;
    }

    if (slab->DeallocateBlock(ptr)) {
        stats_.total_freed_bytes += size;
        stats_.current_used_bytes -= size;
        stats_.allocated_blocks--;
        stats_.free_blocks++;

        LOG_DEBUG("MemoryPool::Deallocate: freed block %zu bytes at %p", size, ptr);
    } else {
        LOG_ERROR("MemoryPool::Deallocate: failed to deallocate block at %p", ptr);
    }
}

MemoryPool::Stats MemoryPool::GetStats() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return stats_;
}

void MemoryPool::Clear() {
    std::lock_guard<std::mutex> lock(mutex_);

    slabs_.clear();
    stats_ = {0, 0, 0, 0, 0, 0};

    LOG_INFO("MemoryPool cleared");
}

} // namespace tinywebserver