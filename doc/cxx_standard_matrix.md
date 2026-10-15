# C++ 标准特性矩阵（实测）

> 生成方式：`bash scripts/feature_matrix.sh <编译器> <标准>`，逐特性 `-fsyntax-only` 试编译。
> 用途：**门禁规则 STD-001 的白名单依据**。任何人修改白名单，必须重跑矩阵并更新本文件。

## 一、测试环境（4 工具链）

| 环境 | 编译器 | 版本 | 标准选项 | `__cplusplus` |
|---|---|---|---|---|
| Windows / MSYS2 UCRT64 | g++ | 16.2.0 | `-std=c++23` | 202302 |
| Windows / MSYS2 UCRT64 | clang++ | 22.1.8 | `-std=c++23` | 202302 |
| WSL2 Ubuntu 24.04 | g++ | 13.3.0 | `-std=c++23` | **202100** |
| WSL2 Ubuntu 24.04 | clang++ | 18.1.3 | `-std=c++23` | 202302 |

> 注意：WSL 的 g++ 13 在 C++23 模式下 `__cplusplus` 仍是 `202100`。
> **因此兼容层的探测一律用 `__cpp_lib_*` / `__cpp_*` 特性宏，绝不用 `__cplusplus` 判断能力。**

## 二、特性矩阵（C++23 模式）

| 特性 | Win g++16 | Win clang22 | WSL g++13 | WSL clang18 | 结论 |
|---|---|---|---|---|---|
| `structured bindings` (C++17) | OK | OK | OK | OK | 可用 |
| `if constexpr` (C++17) | OK | OK | OK | OK | 可用 |
| `std::optional` (C++17) | OK | OK | OK | OK | 可用 |
| `std::filesystem` (C++17) | OK | OK | OK | OK | 可用 |
| `std::string_view` (C++17) | OK | OK | OK | OK | 可用 |
| `concepts` (C++20) | OK | OK | OK | OK | 可用 |
| `std::span` (C++20) | OK | OK | OK | OK | 可用 |
| `std::jthread` (C++20) | OK | OK | OK | OK | 可用 |
| `std::source_location` (C++20) | OK | OK | OK | OK | 可用 |
| `std::format` (C++20) | OK | OK | OK | OK | 可用 |
| designated initializers (C++20) | OK | OK | OK | OK | 可用 |
| `std::bit_cast` (C++20) | OK | OK | OK | OK | 可用 |
| `starts_with/ends_with` (C++20) | OK | OK | OK | OK | 可用 |
| `std::to_underlying` (C++23) | OK | OK | OK | OK | **交集，可用** |
| `std::string::contains` (C++23) | OK | OK | OK | OK | **交集，可用** |
| `std::byteswap` (C++23) | OK | OK | OK | OK | **交集，可用** |
| `std::unreachable` (C++23) | OK | OK | OK | OK | **交集，可用** |
| multivector subscript `m[i,j]` (C++23) | OK | OK | OK | OK | **交集，可用** |
| `if consteval` (C++23) | OK | OK | OK | OK | **交集，可用** |
| `std::expected` (C++23) | OK | OK | OK | **FAIL** | 交集外 |
| `<print>` / `std::print` (C++23) | OK | OK | **FAIL** | **FAIL** | 交集外 |
| `std::mdspan` (C++23) | OK | OK | **FAIL** | **FAIL** | 交集外 |
| `std::flat_map` (C++23) | OK | OK | **FAIL** | **FAIL** | 交集外 |
| `std::stacktrace` (C++23) | OK | OK | OK | OK | 可用（但 Windows 需额外调试库，暂列观察） |
| deducing `this` (C++23) | OK | OK | **FAIL** | OK | 交集外 |
| `views::to` (C++23) | OK | OK | OK* | OK* | 观察（实测为弱断言） |

\* 该行探针较弱（`views::transform` 即可通过），实际使用前需单独验证。

## 三、白名单（可无条件使用）

- 语言：C++17 全部 + C++20 的 `concepts / 指定初始化 / if constexpr`
- 库：`std::optional` `std::string_view` `std::filesystem` `std::span` `std::jthread`
  `std::source_location` `std::format` `std::bit_cast`
- C++23 交集：`std::to_underlying` `std::string::contains` `std::byteswap` `std::unreachable`

## 四、黑名单（必须写进 `#if TWS_HAS_*` 保护块，否则 STD-001 阻断）

| 特性 | 保护宏 | 降级策略 |
|---|---|---|
| `std::expected` | `TWS_HAS_EXPECTED` | 用项目自有 `Error`/`WebError` 体系（本就在用，不新增抽象） |
| `std::print` | `TWS_HAS_PRINT` | 继续用现有日志系统 |
| `std::mdspan` / `std::flat_map` | 同名探测宏 | 暂不使用 |
| `std::stacktrace` | — | 暂不使用 |

## 五、兼容层入口（`include/common/cpp_compat.h`）

调用方只写 `tws::` 前缀，标准差异收敛在一处：

| 入口 | C++23 路径 | 降级路径 |
|---|---|---|
| `tws::to_underlying(E)` | `std::to_underlying` | `static_cast<std::underlying_type_t<E>>` |
| `tws::str_contains(hay, needle)` | —— | 统一 `std::string_view::find`（两套接口差异最大，统一走 find 最稳） |
| `tws::byteswap32(v)` | `std::byteswap` | 手工移位 |

## 六、C++17 兼容性约束（改代码时必须遵守）

1. 不得在**未被 `TWS_HAS_*` 保护**的代码里使用 C++20/23 独占特性（门禁 STD-001 会扫描）。
2. 不得依赖"某个编译器恰好间接引入的头文件"——例如 `int64_t` 必须显式 `#include <cstdint>`
   （该问题在 Clang/MinGW 下直接暴露，是 audit-05 的实际修复项之一）。
3. 头文件必须自洽：任何头文件都要能作为**第一个**被包含的文件独立编译通过。
4. 禁止 `std::aligned_alloc`（MinGW 缺失且对 size 有对齐倍数约束），统一用
   `tws::aligned_alloc_bytes / aligned_free_bytes`（`include/platform/mem_compat.h`）。

## 七、复现命令

```bash
# 特性矩阵（WSL）
wsl -d Ubuntu -- bash /mnt/d/vsc_project/TinyWebServer_demo/scripts/feature_matrix.sh g++ c++23
# 特性矩阵（Windows，MSYS2 UCRT64）
C:\msys64\usr\bin\bash.exe -lc "export PATH=/ucrt64/bin:\$PATH; bash /d/vsc_project/TinyWebServer_demo/scripts/feature_matrix.sh clang++ c++23"

# 构建矩阵（WSL）
bash scripts/verify_matrix.sh g++ 17 wsl-gcc17
bash scripts/verify_matrix.sh clang++ 23 wsl-clang23
# 构建矩阵（Windows，MSYS2 UCRT64）
bash scripts/verify_matrix.sh g++ 23 win-gcc23
```

## 八、平台策略（2026-10-15 决策 B）

Windows 不再提供网络层移植（`epoll/ET/SO_REUSEPORT` 为 Linux 专有；`WSAPoll` 是 O(n)，
高并发需 IOCP = Proactor，违反架构约束）。因此：

- **Windows 构建的是"可移植核心"**：配置 / 日志 / 指标 / 协议解析 / 请求校验 / 内存池 /
  C++ 兼容层，共 17 个源文件 + 9 个 portable 测试；
- Windows 上**不会**出现 `include/platform/win32/**` 这类 POSIX 头遮蔽层（已移除）；
- 交集外特性在 Windows 上同样受 STD-001 约束 —— 因为 Windows 侧也走同一份 compat 层。
