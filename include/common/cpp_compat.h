// =============================================================================
// C++ 标准兼容层（C++17 / 20 / 23 同一份源码）
//
// 三条硬规矩：
//   1) 只探测【四工具链实测交集】内的特性，不造通用 polyfill 库；
//   2) 探测只用 __cpp_lib_* / __cpp_* 特性宏 —— g++13 在 -std=c++23 下
//      __cplusplus 仍是 202100，用它判断能力会误判；
//   3) 交集外特性（expected / print / mdspan / flat_map）必须写在 TWS_HAS_* 保护块内，
//      否则门禁 STD-001 阻断。矩阵与白名单见 doc/cxx_standard_matrix.md。
//
// 实测交集（可无条件使用）：to_underlying、string::contains、byteswap、unreachable。
// =============================================================================
#ifndef TWS_COMMON_CPP_COMPAT_H
#define TWS_COMMON_CPP_COMPAT_H

#include <bit>            // std::byteswap（C++23）
#include <cstdint>        // std::uint32_t
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>        // std::to_underlying（C++23）
// 注意：<bit>/<cstdint>/<utility> 必须显式包含 —— 门禁 QUAL-001 会把每个头文件当
// 独立 TU 编译（-fsyntax-only），依赖间接引入会在"头文件自洽"检查上失败（真实踩过）。

#if defined(__has_include)
#  if __has_include(<version>)
#    include <version>
#  endif
#endif

// ---- 标准版本编号（仅用于日志/诊断，不用于能力判断） ------------------------
#if defined(_MSVC_LANG)
#  define TWS_CPLUSPLUS _MSVC_LANG
#else
#  define TWS_CPLUSPLUS __cplusplus
#endif

// ---- 库特性探测 -------------------------------------------------------------
#if defined(__cpp_lib_to_underlying) && __cpp_lib_to_underlying >= 202102L
#  define TWS_HAS_TO_UNDERLYING 1
#else
#  define TWS_HAS_TO_UNDERLYING 0
#endif

#if defined(__cpp_lib_string_contains) && __cpp_lib_string_contains >= 202011L
#  define TWS_HAS_STRING_CONTAINS 1
#else
#  define TWS_HAS_STRING_CONTAINS 0
#endif

#if defined(__cpp_lib_byteswap) && __cpp_lib_byteswap >= 202110L
#  define TWS_HAS_BYTESWAP 1
#else
#  define TWS_HAS_BYTESWAP 0
#endif

#if defined(__cpp_lib_unreachable) && __cpp_lib_unreachable >= 202202L
#  define TWS_HAS_UNREACHABLE 1
#else
#  define TWS_HAS_UNREACHABLE 0
#endif

// ---- 交集外特性：探测到也只在显式需要时使用（门禁要求加保护） ----------------
#if defined(__cpp_lib_expected) && __cpp_lib_expected >= 202202L
#  define TWS_HAS_EXPECTED 1
#else
#  define TWS_HAS_EXPECTED 0
#endif

#if defined(__cpp_lib_print) && __cpp_lib_print >= 202207L
#  define TWS_HAS_PRINT 1
#else
#  define TWS_HAS_PRINT 0
#endif

// ---- 统一入口（调用方永远写 tws::xxx，标准差异收敛在这里） -------------------
namespace tws {

/// 枚举 -> 底层整数：C++23 起有 std::to_underlying，之前用 static_cast。
template <typename E>
constexpr auto to_underlying(E value) noexcept -> std::underlying_type_t<E> {
#if TWS_HAS_TO_UNDERLYING
    return std::to_underlying(value);
#else
    return static_cast<std::underlying_type_t<E>>(value);
#endif
}

/// 子串包含：统一走 find，避免 std::string / std::string_view 两套接口差异。
constexpr bool str_contains(std::string_view haystack, std::string_view needle) noexcept {
    return haystack.find(needle) != std::string_view::npos;
}

/// 字节序翻转：C++23 起有 std::byteswap，之前用逐位移位实现。
constexpr std::uint32_t byteswap32(std::uint32_t value) noexcept {
#if TWS_HAS_BYTESWAP
    return std::byteswap(value);
#else
    return ((value & 0x000000ffu) << 24) | ((value & 0x0000ff00u) << 8) |
           ((value & 0x00ff0000u) >> 8)  | ((value & 0xff000000u) >> 24);
#endif
}

}  // namespace tws

#endif  // TWS_COMMON_CPP_COMPAT_H
