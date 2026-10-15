// =============================================================================
// C++ 兼容层与跨平台小工具单元测试（portable：Windows 与 Linux 都必须通过）
//
// 为什么存在：门禁规则 TEST-002 要求"新增能力必须带测试"。本测试覆盖两类新增能力：
//   1. C++ 标准兼容层（include/common/cpp_compat.h）：C++17/20/23 同一份源码；
//   2. 跨平台小工具（include/platform/mem_compat.h、fs_compat.h）：
//      MinGW 缺 std::aligned_alloc、mkdir 元数不同。
// 刻意不碰 socket/epoll：网络层是 Linux 专有（见 SPEC.md 平台策略）。
// =============================================================================
#include "common/cpp_compat.h"
#include "platform/fs_compat.h"
#include "platform/mem_compat.h"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <string_view>

namespace {

int g_checks = 0;

void Check(bool condition, const char* what) {
    ++g_checks;
    if (!condition) {
        std::fprintf(stderr, "[FAIL] %s\n", what);
        assert(condition);
    }
    std::printf("  [ok] %s\n", what);
}

enum class Color : unsigned char { kRed = 1, kGreen = 2 };

void TestCppCompat() {
    std::printf("[case] C++ 标准兼容层\n");
    Check(tws::to_underlying(Color::kGreen) == 2u, "to_underlying 取到枚举底层值");
    Check(tws::str_contains("TinyWebServer", "Web"), "str_contains 命中子串");
    Check(!tws::str_contains("TinyWebServer", "zzz"), "str_contains 未命中返回 false");
    Check(tws::str_contains(std::string_view{}, ""), "空串包含空串");
    Check(tws::byteswap32(0x11223344u) == 0x44332211u, "byteswap32 字节序翻转正确");
    Check(tws::byteswap32(tws::byteswap32(0xDEADBEEFu)) == 0xDEADBEEFu, "byteswap32 自反");
}

void TestAlignedAlloc() {
    std::printf("[case] 对齐分配\n");
    constexpr std::size_t kAlign = 64;
    void* ptr = tws::aligned_alloc_bytes(kAlign, 4096);
    Check(ptr != nullptr, "aligned_alloc_bytes 返回非空");
    Check(reinterpret_cast<std::uintptr_t>(ptr) % kAlign == 0, "返回地址满足 64 字节对齐");
    std::memset(ptr, 0xAB, 4096);   // 可写：证明这块内存确实可访问
    Check(static_cast<unsigned char*>(ptr)[4095] == 0xAB, "整块内存可读写");
    tws::aligned_free_bytes(ptr);

    // 小于指针宽度的对齐请求必须被提升，而不是返回未对齐内存
    void* small = tws::aligned_alloc_bytes(1, 64);
    Check(small != nullptr, "过小对齐请求被提升后仍可分配");
    Check(reinterpret_cast<std::uintptr_t>(small) % sizeof(void*) == 0, "提升后满足指针宽度对齐");
    tws::aligned_free_bytes(small);

    tws::aligned_free_bytes(nullptr);   // 空指针安全
    Check(true, "aligned_free_bytes(nullptr) 安全");
}

void TestMakeDir() {
    std::printf("[case] 目录创建\n");
    const auto base = std::filesystem::temp_directory_path() / "tws_cpp_compat_test";
    std::error_code ec;
    std::filesystem::remove_all(base, ec);
    const std::string path = base.string();
    Check(tws::make_dir(path.c_str(), 0755) == 0, "新建目录返回 0");
    Check(std::filesystem::is_directory(base), "目录确实存在");
    Check(tws::make_dir(path.c_str(), 0755) != 0, "重复创建返回非 0（语义明确，不假装成功）");
    std::filesystem::remove_all(base, ec);
}

}  // namespace

int main() {
    std::printf("==== test_cpp_compat ====\n");
    TestCppCompat();
    TestAlignedAlloc();
    TestMakeDir();
    std::printf("==== PASSED (%d checks) ====\n", g_checks);
    return 0;
}
