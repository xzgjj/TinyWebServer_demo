# CLI —— 命令契约

预算：≤3000 字（非空白字符）。所有命令在 Windows(MSYS2 UCRT64) 与 WSL2 行为一致。

## 1. 构建

```bash
cmake -S . -B build -G Ninja \
  -DTWS_CXX_STANDARD=23 \      # 17 | 20 | 23（默认 23，严格模式无 gnu 扩展）
  -DCMAKE_BUILD_TYPE=Debug \   # Debug | Release
  -DBUILD_TESTING=ON \         # 关掉则只构建库与 server
  -DBUILD_BENCHMARKS=ON \      # 基准框架
  -DENABLE_ASAN=OFF \          # POSIX Debug 下默认 ON；Windows 默认 OFF
  -DTWS_WERROR=OFF \           # ON 时警告即错误（CI 严格模式）
  -DTWS_NATIVE_ARCH=OFF        # ON 时加 -march=native（仅本机用）
cmake --build build --parallel
```

多平台要点（平台策略见 SPEC.md 第 2 节）：
- **Linux/WSL2**：全量目标（`server` + 全部测试 + `benchmark_runner`）。
- **Windows**：只构建可移植核心 + 9 个 portable 测试；`server`/`reactor/**`/`http2`/
  `plugin`/基准框架被显式跳过，CMake 会打印跳过原因。
- `build/compile_commands.json` 供 clangd / VS Code 问题面板使用。

## 2. 测试

```bash
ctest --test-dir build --output-on-failure        # 全量
ctest --test-dir build -L smoke                   # 快速冒烟
ctest --test-dir build -L portable                # 跨平台集（Windows 上就是全部测试）
ctest --test-dir build -L socket                  # 网络集成测试（仅 Linux 构建）
```

## 3. 辅助脚本

```bash
python tools.py clean                 # 清理构建目录
python tools.py build --mode debug    # 配置并构建（Ninja 优先）
python tools.py test                  # 跑测试并生成 audit_report.md
python tools.py ci-local --std 23     # 构建 + ctest + 门禁判定
python tools.py debug --target test_basic
python tools.py benchmark baseline    # 建立性能基线
python tools.py clean-logs --dry-run  # 日志清理预演
```

## 4. 门禁（`python tools.py gate ...` 等价于 `python scripts/gate/cli.py ...`）

```bash
gate declare --task v8 --title "..." --scope "src/**,include/**" \
             --budget 800 --max-files 24 --accept "四工具链全绿;ctest 通过"
gate run [--task v8] [--base origin/main..HEAD] [--serial] [--skip-heavy] [--json]
gate status
gate explain QUAL-002
gate report --run last
gate exception request --rule SCOPE-002 --reason "..." [--ttl-days 30]
gate exception approve --id W-20261015-001 --by xzgjj
gate exception reject  --id W-20261015-001 --by xzgjj --reason "..."
gate exception list --pending
gate reset --task v8 --reason "已定位并修复根因"
gate selftest [--strict]
```

- **判定区间**：默认按工作区相对 `HEAD`；CI 用 `--base A..B`。
- **退出码**：`0` 通过 / `1` 阻断 / `2` 重试超限(HALTED) / `3` 规格错误 / `4` 环境错误。
- **重试上限**：同一任务连续失败达 `meta.max_gate_attempts`（默认 3）即 HALTED；
  先 `gate reset` 才能继续（防止一次任务无限自动重试）。
- **例外**：`approved_by` 必须 ≠ 变更作者，且属于 `meta.rule_approvers`；
  未批准的例外按阻断处理（fail-closed）。
- **受保护路径**：`gate/spec.toml`、`gate/baseline.json`、`gate/protected_tests.json`、
  `scripts/gate/`、`.github/workflows/`、`CODEOWNERS`。改动需要非作者批准的 waiver；
  GitHub 侧由 `CODEOWNERS` + 分支保护强制。

## 5. 报告与产物

```
local/gate/<run_id>/report.json     # 机器可读（含 observed/threshold/evidence）
local/gate/<run_id>/report.md       # 人类可读（阻断项/例外项/警告项/转人工项/证据索引）
local/gate/<run_id>/evidence/       # diffstat.txt、lint.txt、cppcheck.txt
gate/ledger.jsonl                   # 审计账本（每次运行一行）
gate/waivers.jsonl                  # 例外账本（request/approve/reject 事件）
```

## 6. 复现构建矩阵

```bash
# WSL2
bash scripts/verify_matrix.sh g++ 17 wsl-gcc17
bash scripts/verify_matrix.sh clang++ 23 wsl-clang23
# Windows（MSYS2 UCRT64）
bash scripts/verify_matrix.sh g++ 23 win-gcc23
# C++ 标准特性矩阵（26 特性 × 工具链）
bash scripts/feature_matrix.sh g++ c++23
```
