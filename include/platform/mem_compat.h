// =============================================================================
// 对齐内存分配兼容层
//
// 背景：std::aligned_alloc 是 C11/C++17 接口，但 MinGW-w64 的 <cstdlib> 不提供它
// （Windows 平台用 _aligned_malloc/_aligned_free 这一对）。
// 另外 std::aligned_alloc 要求 size 是 alignment 的整数倍，否则是 UB；
// 这里改用 posix_memalign（无该约束），语义更安全。
// =============================================================================
#ifndef TWS_PLATFORM_MEM_COMPAT_H
#define TWS_PLATFORM_MEM_COMPAT_H

#include <cstddef>

#ifdef TWS_PLATFORM_WIN32
#include <malloc.h>
#else
#include <cstdlib>
#endif

namespace tws {

/// 对齐分配；失败返回 nullptr（调用方按 bad_alloc 处理）。
inline void* aligned_alloc_bytes(std::size_t alignment, std::size_t size) {
    if (alignment < sizeof(void*)) {
        alignment = sizeof(void*);
    }
#ifdef TWS_PLATFORM_WIN32
    return _aligned_malloc(size, alignment);
#else
    void* ptr = nullptr;
    if (posix_memalign(&ptr, alignment, size) != 0) {
        return nullptr;
    }
    return ptr;
#endif
}

/// 与 aligned_alloc_bytes 配对释放（Windows 上必须用 _aligned_free）。
inline void aligned_free_bytes(void* ptr) {
    if (ptr == nullptr) {
        return;
    }
#ifdef TWS_PLATFORM_WIN32
    _aligned_free(ptr);
#else
    std::free(ptr);
#endif
}

}  // namespace tws

#endif  // TWS_PLATFORM_MEM_COMPAT_H
