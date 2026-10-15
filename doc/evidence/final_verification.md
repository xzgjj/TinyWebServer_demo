# v8 最终验收证据（本机实跑）

生成时间：2026-10-15 · 分支 `feat/v8-cpp23-gates` · 基线提交 `ae1eaa5`（origin/main）

## 1. 构建与测试矩阵（8 格，全部本机实跑）

命令（CI 与本地共用同一套，避免"CI 绿本地红"）：
`bash scripts/verify_matrix.sh <编译器> <标准> <tag>`

| 工具链 | c++17 | c++23 |
|---|---|---|
| WSL2 g++ 13.3.0 | build rc=0 · **100% tests passed** | build rc=0 · **100% tests passed** |
| WSL2 clang++ 18.1.3 | build rc=0 · **100% tests passed** | build rc=0 · **100% tests passed** |
| Windows g++ 16.2.0 (MSYS2 UCRT64) | build rc=0 · **9/9 portable 通过** | build rc=0 · **9/9 portable 通过** |
| Windows clang++ 22.1.8 (MSYS2 UCRT64) | build rc=0 · **9/9 portable 通过** | build rc=0 · **9/9 portable 通过** |

- WSL 全量测试数：21（9 portable + 12 socket/集成）
- Windows 测试数：9（全部 portable；网络层按平台策略不构建，CMake 会打印跳过原因）
- 警告数（用于 QUAL-001 棘轮基线）：WSL g++ 176–370 / clang++ 796–802；Windows g++ 47–49 / clang++ 111–114

## 2. 门禁规则负向回归（`selftest`：该红时必须红）

| 环境 | 普通模式 | `--strict` |
|---|---|---|
| Windows（有 cppcheck） | 17/17 | **17/17，零 SKIP** |
| WSL2（无 cppcheck） | 17/17 | 16/17，QUAL-003 显式 SKIP（不是通过） |

SKIP 语义：夹具依赖的工具缺失时，"无法验证"与"规则失效"必须区分；
但 SKIP 绝不算通过 —— CI 用 `--strict` 保证零 SKIP。

## 3. 真实仓库门禁判定（`--base ae1eaa5..HEAD`）

```
✓/✗ 门禁判定  task=v8-cpp23-gates  commit=21870b7  branch=feat/v8-cpp23-gates
规则统计：pass 12 / warn 4 / block 1
```

- **唯一阻断项 CI-001**：`CODEOWNERS, gate/baseline.json, gate/protected_tests.json, gate/spec.toml`
  受保护路径变更缺少有效批准。这是**规则按设计生效**（规则文件不得由写作者自己放行）。
  已发起待批准例外 `W-261015-2722`，未批准前规则仍按原样阻断（fail-closed）。
- 警告项（均已冻结/显式标注，不阻断）：
  - QUAL-002 三个存量超标函数已冻结在 `gate/baseline.json`（只允许下降）
  - TEST-001 新增受保护测试文件 `test/test_cpp_compat.cpp`（新增，不是修改）
  - DOC-003 文档中提到裸文件名 `package-lock.json`（无法判定是否仓库内路径，降级为 WARN）
  - QUAL-003 本机缺 cppcheck（按"未验证"处理，不假装通过）

## 4. 门禁在本次改造中真实抓到的缺陷（自证有效）

| # | 门禁规则 | 抓到的真实问题 | 处置 |
|---|---|---|---|
| 1 | QUAL-001 | `cpp_compat.h` 用了 `std::to_underlying`/`std::uint32_t` 却没显式包含 `<utility>`/`<cstdint>`（头文件不自洽：作为独立 TU 编译即失败） | 补 include，写进注释与矩阵文档 |
| 2 | CODE-001 | 兼容层中未被引用的 `static` 函数（`ntohs`/`ntohl`/`sendto`）与超长注释代码块 | 删除未用函数、缩短注释块 |
| 3 | DOC-003 | SPEC.md/CLI.md 引用了尚未创建的 `doc/gate_system_design.md`、`gate/baseline.json` | 补齐文件 |
| 4 | QUAL-002/QUAL-001 | 存量超标函数与警告无基线 → 门禁永久红 | 引入棘轮 `gate/baseline.json` |
| 5 | SCOPE-001 | 任务契约 allow 未覆盖 `SPEC.md`/`tools.py`（契约滞后于实际改动） | 修订契约（revision+1） |
| 6 | 门禁自身 | Windows 上 git 不可用时"改动集为空 → SCOPE 判定静默通过"（看起来通过、其实没检查） | `GitFacts.available` + SCOPE/diff_budget 在 git 不可用时返回 ERROR |
| 7 | 门禁自身 | 负向夹具被当成产品代码扫描（QUAL-002/STD-001 报夹具"违规"） | `meta.exclude_globs` 增加 `gate/fixtures/**` |
| 8 | 门禁自身 | `selftest.py` 内嵌规格与 `spec.toml` 漂移（改 spec 未同步镜像） | 同步镜像（该校验本身就是防漂移设计） |

## 5. 已知边界（诚实声明，不掩饰）

1. **本地无法强制"批准者≠作者"**：本地 git 身份只有 `xzgjj`，
   代理与人类作者不可区分；真正的强制在 GitHub 分支保护 + `CODEOWNERS`。
2. **无判定器级超时**：Python 线程不可强杀；判定器内部用 `ctx.run(timeout=…)` 兜底。
3. **Windows 只覆盖可移植核心**（SPEC 第 2 节平台策略）：`server`/`reactor`/`http2`/
   `plugin`/benchmark 在 Windows 上不构建，属**有意为之**，不是未完成。
4. **警告基线按编译器名分桶**，未按"平台×编译器"分桶；跨平台数值差异靠棘轮容纳。
