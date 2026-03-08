#include "memory_pool.h"
#include <cassert>
#include <iostream>
#include <vector>
#include <thread>
#include <atomic>

using namespace tinywebserver;

void TestBasicAllocation() {
    std::cout << "=== TestBasicAllocation ===" << std::endl;

    auto& pool = MemoryPool::GetInstance();

    // 分配不同大小的内存
    void* small = pool.Allocate(32);  // SMALL类
    assert(small != nullptr);

    void* medium = pool.Allocate(128); // MEDIUM类
    assert(medium != nullptr);

    void* large = pool.Allocate(512);  // LARGE类
    assert(large != nullptr);

    void* huge = pool.Allocate(2048); // HUGE类
    assert(huge != nullptr);

    // 释放内存
    pool.Deallocate(small, 32);
    pool.Deallocate(medium, 128);
    pool.Deallocate(large, 512);
    pool.Deallocate(huge, 2048);

    std::cout << "Basic allocation test passed!" << std::endl;
}

void TestReuse() {
    std::cout << "=== TestReuse ===" << std::endl;

    auto& pool = MemoryPool::GetInstance();

    // 分配和释放多次，验证内存被重用
    std::vector<void*> allocations;

    for (int i = 0; i < 10; ++i) {
        void* ptr = pool.Allocate(64);
        assert(ptr != nullptr);
        allocations.push_back(ptr);
    }

    // 释放一半
    for (size_t i = 0; i < allocations.size(); i += 2) {
        pool.Deallocate(allocations[i], 64);
        allocations[i] = nullptr;
    }

    // 再次分配，应该重用已释放的内存
    for (size_t i = 0; i < allocations.size(); i += 2) {
        allocations[i] = pool.Allocate(64);
        assert(allocations[i] != nullptr);
    }

    // 清理
    for (void* ptr : allocations) {
        if (ptr) {
            pool.Deallocate(ptr, 64);
        }
    }

    std::cout << "Memory reuse test passed!" << std::endl;
}

void TestStats() {
    std::cout << "=== TestStats ===" << std::endl;

    auto& pool = MemoryPool::GetInstance();

    auto stats_before = pool.GetStats();
    size_t initial_allocated = stats_before.total_allocated_bytes;

    void* ptr1 = pool.Allocate(100);
    void* ptr2 = pool.Allocate(200);
    void* ptr3 = pool.Allocate(1025); // HUGE

    auto stats_mid = pool.GetStats();
    assert(stats_mid.current_used_bytes > 0);
    assert(stats_mid.allocated_blocks >= 3);

    pool.Deallocate(ptr1, 100);
    pool.Deallocate(ptr2, 200);
    pool.Deallocate(ptr3, 1025);

    auto stats_after = pool.GetStats();
    assert(stats_after.current_used_bytes == 0);
    assert(stats_after.total_freed_bytes >= stats_mid.total_allocated_bytes - initial_allocated);

    std::cout << "Stats test passed!" << std::endl;
    std::cout << "  Total allocated: " << stats_after.total_allocated_bytes << " bytes" << std::endl;
    std::cout << "  Total freed: " << stats_after.total_freed_bytes << " bytes" << std::endl;
    std::cout << "  Current used: " << stats_after.current_used_bytes << " bytes" << std::endl;
}

void TestThreadSafety() {
    std::cout << "=== TestThreadSafety ===" << std::endl;

    auto& pool = MemoryPool::GetInstance();
    std::atomic<int> success_count{0};
    const int num_threads = 4;
    const int allocs_per_thread = 100;

    auto worker = [&pool, &success_count, allocs_per_thread](int thread_id) {
        std::vector<void*> allocations;
        allocations.reserve(allocs_per_thread);

        for (int i = 0; i < allocs_per_thread; ++i) {
            size_t size = (i % 4) * 64 + 32; // 不同大小
            void* ptr = pool.Allocate(size);
            if (ptr) {
                allocations.push_back(ptr);
                // 简单写入验证
                *static_cast<char*>(ptr) = static_cast<char>(thread_id);
            }
        }

        // 释放
        for (size_t i = 0; i < allocations.size(); ++i) {
            size_t size = (i % 4) * 64 + 32;
            pool.Deallocate(allocations[i], size);
        }

        success_count++;
    };

    std::vector<std::thread> threads;
    for (int i = 0; i < num_threads; ++i) {
        threads.emplace_back(worker, i);
    }

    for (auto& t : threads) {
        t.join();
    }

    assert(success_count == num_threads);
    std::cout << "Thread safety test passed! (" << num_threads << " threads)" << std::endl;
}

void TestEdgeCases() {
    std::cout << "=== TestEdgeCases ===" << std::endl;

    auto& pool = MemoryPool::GetInstance();

    // 分配0字节
    void* zero = pool.Allocate(0);
    assert(zero == nullptr);

    // 分配超大内存
    void* huge = pool.Allocate(10 * 1024 * 1024); // 10MB
    assert(huge != nullptr);
    pool.Deallocate(huge, 10 * 1024 * 1024);

    // 重复释放（应该安全处理）
    void* ptr = pool.Allocate(64);
    assert(ptr != nullptr);
    pool.Deallocate(ptr, 64);
    pool.Deallocate(ptr, 64); // 第二次释放，应该被安全处理

    std::cout << "Edge cases test passed!" << std::endl;
}

int main() {
    std::cout << "Starting MemoryPool tests..." << std::endl;

    try {
        TestBasicAllocation();
        TestReuse();
        TestStats();
        TestThreadSafety();
        TestEdgeCases();

        // 清理内存池
        MemoryPool::GetInstance().Clear();

        std::cout << "\nAll MemoryPool tests passed!" << std::endl;
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "Test failed with exception: " << e.what() << std::endl;
        return 1;
    }
}