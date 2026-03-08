#pragma once

#include <cstddef>
#include <vector>
#include <mutex>
#include <unordered_map>
#include <memory>

namespace tinywebserver {

/**
 * @brief 分级内存池
 *
 * 内存分配优化，减少系统调用和内存碎片
 * 支持四种大小类别：
 *   - SMALL:  <= 64B
 *   - MEDIUM: <= 256B
 *   - LARGE:  <= 1KB
 *   - HUGE:   > 1KB (直接使用系统分配器)
 */
class MemoryPool {
public:
    /// 内存大小类别
    enum class SizeClass {
        SMALL = 64,      // <= 64B
        MEDIUM = 256,    // <= 256B
        LARGE = 1024,    // <= 1KB
        HUGE             // > 1KB，直接使用系统分配器
    };

    /**
     * @brief 获取内存池单例实例
     */
    static MemoryPool& GetInstance();

    /**
     * @brief 分配指定大小的内存
     * @param size 请求的字节数
     * @return 分配的内存指针，失败返回nullptr
     */
    void* Allocate(size_t size);

    /**
     * @brief 释放内存
     * @param ptr 要释放的内存指针
     * @param size 释放的字节数（用于确定大小类别）
     */
    void Deallocate(void* ptr, size_t size);

    /**
     * @brief 获取内存池统计信息
     */
    struct Stats {
        size_t total_allocated_bytes;      // 总分配字节数
        size_t total_freed_bytes;          // 总释放字节数
        size_t current_used_bytes;         // 当前使用字节数
        size_t slab_count;                 // Slab数量
        size_t free_blocks;                // 空闲块数量
        size_t allocated_blocks;           // 已分配块数量
    };

    Stats GetStats() const;

    /**
     * @brief 清空内存池（释放所有内存）
     * 主要用于测试和清理
     */
    void Clear();

private:
    MemoryPool();
    ~MemoryPool();
    MemoryPool(const MemoryPool&) = delete;
    MemoryPool& operator=(const MemoryPool&) = delete;

    /// Slab内存块
    struct Slab {
        void* memory;               // 内存块起始地址
        size_t block_size;          // 每个块的大小
        size_t block_count;         // 块数量
        std::vector<bool> used;     // 块使用状态
        size_t free_count;          // 空闲块数量

        Slab(size_t block_size, size_t block_count);
        ~Slab();

        void* AllocateBlock();
        bool DeallocateBlock(void* ptr);
        bool Contains(void* ptr) const;
    };

    /// 确定请求大小所属的类别
    SizeClass GetSizeClass(size_t size) const;

    /// 为指定类别创建新的Slab
    Slab* CreateSlab(SizeClass size_class);

    /// 查找包含指定指针的Slab
    Slab* FindSlabContaining(void* ptr);

    // 按类别组织的Slab列表
    std::unordered_map<SizeClass, std::vector<std::unique_ptr<Slab>>> slabs_;

    // 保护并发访问
    mutable std::mutex mutex_;

    // 统计信息
    Stats stats_;
};

} // namespace tinywebserver