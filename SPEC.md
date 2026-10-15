# SPEC —— TinyWebServer v8 规格与验收规范

预算：≤3000 字（非空白字符）。超出即被门禁 DOC-001 阻断。
实现细节、设计推理、交互全流程见 `doc/gate_system_design.md`（不受限）。

## 1. 目标

1. **标准**：以 C++23 为默认，同一份源码在 C++17 下同样构建通过。
2. **平台**：Windows（MSYS2 UCRT64）与 WSL2（Ubuntu）原生构建通过。
3. **门禁**：把"可判定的工程纪律"变成机器红/绿，人只负责"值不值得"的判断。

非目标：不引入第三方运行时依赖；不做 MSVC 支持；不追求协议完备性扩展。

## 2. 平台与编译器策略（2026-10-15 决策 B）

本项目的技术身份建立在 Linux 专有机制上：`epoll`（O(1) 事件通知）、**边缘触发 ET**、
`SO_REUSEPORT`（内核级负载均衡）、`eventfd`、`mmap` 零拷贝。
Windows 没有等价物：`WSAPoll` 是 O(n) 轮询，要拿到同等并发只能上 IOCP，
而 IOCP 是 Proactor 模型，直接违反「禁止从 Reactor 切到 Proactor」的架构约束。

因此**不提供 Windows 原生 server**，而是按可移植性二分源码：

- **Linux / WSL2**：一等公民。全量构建（库 + server + 全部测试 + 基准）与全量 ctest。
- **Windows**：只构建并测试**平台无关核心**（配置 / 日志 / 指标 / 协议解析 / 请求校验 /
  内存池 / C++ 兼容层），`server`、`reactor/**`、`http2`、`plugin`、基准框架显式跳过，
  并在 CMake 输出里说明原因（不静默降级）。

| 工具链 | c++17 | c++23 |
|---|---|---|
| WSL2 g++ 13.3 | 全量构建 + 全量 ctest | 全量构建 + 全量 ctest |
| WSL2 clang++ 18.1 | 全量构建 + 全量 ctest | 全量构建 + 全量 ctest |
| Windows g++ 16.2 | 可移植核心 + 9 个 portable 测试 | 同左 |
| Windows clang++ 22.1 | 可移植核心 + 9 个 portable 测试 | 同左 |

- 可用特性白名单由 `doc/cxx_standard_matrix.md` 的实测矩阵决定（交集内无条件可用）。
- 该策略使"多编译器"成为唯一需要保证的维度：g++ 与 clang 在两种 OS 上都必须零错误。

## 3. 门禁契约

规则唯一真相源 `gate/spec.toml`。每条规则：机器判定（block/warn/manual）+ 人工判断。

| 规则 | 机器判定 | 人工判断 |
|---|---|---|
| DOC-001 | 新文档 ≤3000 字；存量文档棘轮（只减不增） | 内容是否值得保留 |
| DOC-002 | 段内重复块=0；跨文档重复 ≤5% | 抽象是否必要 |
| DOC-003 | 引用路径存在；文档标准与 CMake 一致 | 设计一致性 |
| CODE-001 | 无注释代码块/未用私有成员/未用静态函数 | 抽象必要性 |
| CODE-002 | 重复代码窗口 ≤2（warn） | — |
| SCOPE-001 | 改动必须落在任务契约 allow 内且不碰 deny | 越界批准 |
| SCOPE-002 | 单任务改动 ≤ 契约预算（默认 800 行 / 24 文件） | 超预算批准 |
| QUAL-001 | 编译错误=0；新增警告 ≤ 基线 | 命名审查 |
| QUAL-002 | 圈复杂度 ≤15、函数 ≤80 行、嵌套 ≤4（存量棘轮） | 设计审查 |
| QUAL-003 | compile_commands.json 存在；cppcheck ≤ 基线 | — |
| STD-001 | 交集外特性必须写在 `TWS_HAS_*` 保护块内 | — |
| TEST-001 | 受保护测试哈希/用例数变化 → 转人工 | 行为变更是否合理 |
| TEST-002 | 新增源文件必须有对应测试且 smoke 通过 | — |
| CI-001 | 受保护路径变更需 waiver，且批准人 ≠ 作者 | 批准者与开发者分离 |
| GOV-001 | 同任务失败 ≥3 次 → HALTED，禁止自动重试 | 人工 reset |
| GOV-002 | 每次运行必须写审计账本 | — |

阈值一律从 spec 读取，代码内不得硬编码。

## 4. 交互契约

任务状态：`DECLARED → RUNNING → PASSED|FAILED → (EXCEPTION_REQUESTED) → HALTED`。
每次运行打印：规则 / 状态 / 实测 vs 阈值 / 修复提示；报告区分机器判定与人工判断。

退出码：`0` 通过、`1` 阻断、`2` 重试超限、`3` 规格错误、`4` 环境错误。

## 5. 数据契约

| 文件 | 性质 |
|---|---|
| `gate/spec.toml` | 规则（受保护，改动需非作者审批） |
| `gate/tasks/<id>.json` | 任务契约：范围、预算、验收标准、尝试次数 |
| `gate/waivers.jsonl` | 例外账本（append-only，含批准人） |
| `gate/ledger.jsonl` | 审计账本（append-only） |
| `gate/baseline.json` | 存量棘轮基线（字数/警告/复杂度/cppcheck） |
| `gate/protected_tests.json` | 受保护测试哈希基线 |
| `local/gate/<run>/report.{json,md}` | 运行报告与证据（不入库） |

## 6. 验收标准（可量化）

1. 第 2 节矩阵 8 格全绿（实跑，非推断）。
2. WSL2 全量 `ctest` 通过；Windows `ctest` 全绿（Windows 上只有 portable 测试目标）。
3. `python scripts/gate/selftest.py --strict` 16 条规则负向回归全过、零 SKIP。
4. `gate run` 可跑通并产出 json+md 报告；未批准例外一律计为阻断（fail-closed）。
5. VS Code Problems 面板 0 error（compile_commands.json 可用）。
6. 每个提交 500–800 行量级、逻辑完整可运行、本地提交不推送，经用户确认后推送。
