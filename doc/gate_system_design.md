# 门禁系统设计（交互 · 接口 · 数据 · 最坏情况 · 落地性复检）

> 本文是**分析/设计类文档，不设字数上限**。预算内文档（SPEC.md / CLI.md /
> AGENT_EXECUTION_PROTOCOL.md / CLAUDE.md / implementation_plan.md）只保留结论。
> 面向读者：需要照此实现或维护门禁的人（含 AI 代理）。

## 0.1 四处"设计缺陷"的修正（2026-10-15，实测驱动）

第一版门禁在真实仓库上跑不动，根因不是操作失误，而是四个**设计漏洞**。它们的共性是：
规则只考虑了"正常期"，没考虑**相变**（从 0 到 1 的那一刻）。

| # | 缺陷 | 为什么是设计问题 | 修正 |
|---|---|---|---|
| 1 | **自举死锁**：规则体系由"被它自己保护的改动"引入，此时"非作者批准"无解——唯一审批人就是写作者 | 规则没有"引入期"与"运行期"的区分 | 新增**自举期**语义：spec 与引擎入口在比对基线里都不存在（状态 A）时，CI-001 对受保护改动**降级为 WARN**（照常记账、要求人类合并时确认），合并后自动恢复阻断。判据是事实（git 状态）而非自我声明 |
| 2 | **原地重试 vs 正常迭代**：原实现"只要失败就 +1"，于是"改一版跑一次"也被判成重试循环，第 4 次 HALTED | 防循环的正确对象是"**修订不变**的重复失败"，不是"失败次数" | `attempts` 改为**同修订连续失败才累加**（指纹 = 提交 + 规格哈希 + 工作区改动，且排除门禁自身记账文件）。改代码即归 1，原样重跑才累加 |
| 3 | **告警被当成阻断**：受保护测试"内容被修改"直接 MANUAL 阻断 | "加强断言"与"放宽判定"都是内容变化，**只有人能判断**；硬阻断会把正常改进逼成走例外，反而鼓励关规则 | TEST-001 改三态：**删除测试 / 用例数下降 → 硬阻断**（机器可判定）；**仅内容修改 → WARN + 提请人审** |
| 4 | **预算可被自己放宽**：契约文件由代理自己写，改大 `max_changed_lines` 即可通过 | 规则不能依赖"被约束者自觉" | 新增硬上限 `max_changed_lines_cap / max_changed_files_cap`（写在受保护的 spec 里）：契约只能收紧、不能放宽；要突破必须改规则 → 触发 CI-001 |

配套还修了三处"看得见的不合理"：waiver 与 commit 死绑（审批后 HEAD 前进即静默失效）、
`exception request` 不带范围导致例外永不生效、报告不列出"被豁免的具体条目"。

## 0. 为什么需要这套东西（问题定义）

AI 代理与人类在同一个仓库里写代码，双方的行为偏差不是"能力问题"，而是**激励与视角差异**：

| 偏差 | 代理侧成因 | 只靠"提醒"为什么无效 |
|---|---|---|
| 文档膨胀 | 生成成本低、显得完整 | 每份文档单看都合理，累积后没人读 |
| 堆细节/造抽象 | 更容易"加一层"而不是删一层 | 抽象是否必要是判断，不是事实 |
| 小任务变大重构 | 顺手修一切看到的坏味道 | 范围边界没有机器记录 |
| 代码能跑但读不懂 | 通过编译不等于可维护 | 可读性是判断，但复杂度是事实 |
| 改测试让失败消失 | 目标是"变绿"而不是"变对" | 测试文件本身没人保护 |
| 顺手关规则 | 规则挡路就想改规则 | 规则文件与开发者同权限 |

因此设计目标不是"让代理更自觉"，而是：**把可判定的部分变成机器红/绿，把不可判定的部分
变成"必须由另一个人签字"的动作**，并且两个动作都留痕。

## 1. 交互设计（角色 · 路径 · 状态 · 反馈）

### 1.1 角色

- **Agent（自治写作者）**：产出快、易越界、会把"变绿"当作目标。
- **Developer（人类写作者）**：与 Agent 同类，但会判断取舍。
- **Approver（人类审批者）**：**必须 ≠ 写作者**；规则文件变更的唯一合法批准人。

### 1.2 主路径 P1：Agent 领任务 → 提交

| 步 | 触发动作 | 系统状态变化 | 参与者看到的反馈 |
|---|---|---|---|
| 1 | `gate declare --task v8 --scope "src/**" --budget 800` | 生成 `gate/tasks/v8.json`（scope/预算/验收标准/attempts=0），state=`DECLARED` | 契约摘要 + 下一步命令 |
| 2 | 写代码 | — | — |
| 3 | `gate run` | state→`RUNNING`；采集 git 事实 → 并发跑 16 条判定器 | 逐条：`✓ DOC-001` / `✗ SCOPE-001 越界: third_party/json.hpp` |
| 4 | 失败 | state→`FAILED`，`attempts += 1` | 每条 Finding 含：规则 / 文件:行 / **实测 vs 阈值** / 修复提示 / 证据路径 |
| 5 | 修复后重跑 | 同上 | 同一 `run_id` 语义：attempt 递增可见 |
| 6 | 连续失败达上限 | state→`HALTED`，退出码 2 | `⛔ 已连续失败 3 次，停止自动重试；需人工 gate reset` |
| 7 | 通过 | state→`PASSED`，attempts 归零 | 报告路径 + 建议的 commit 命令 |
| 8 | 提交 | state→`COMMITTED`（仅本地） | CI 摘要 |
| 9 | 用户确认后推送 | — | 见 §6 交付纪律 |

### 1.3 例外路径 P2：申请 → 批准

```
写作者: gate exception request --rule SCOPE-002 --reason "CMake 平台层必须同批落地"
        → gate/waivers.jsonl 追加 pending 记录（approved_by 为空）
        → 门禁仍按阻断处理（fail-closed，不因"已申请"就放行）
审批者: gate exception approve --id W-... --by xzgjj
        → 校验 approver ≠ requested_by、approver ∈ meta.rule_approvers、未过期
        → 该 waiver 生效：对应规则由 ✗ block 变为 ⊘ waived，报告单列
```
**为什么申请不等于放行**：否则"申请"会变成绕过门禁的常规动作。

### 1.4 红线路径 P3：改规则

改 `gate/spec.toml` / `baseline.json` / `protected_tests.json` / `scripts/gate/**` /
`.github/workflows/**` / `CODEOWNERS` → 触发 `CI-001` → 需要非作者批准的 waiver。
GitHub 侧由 `CODEOWNERS` + 分支保护强制；本地只能留痕（**不假装本地能强制**）。

### 1.5 反馈状态词表与退出码

`✓ pass` / `✗ block` / `⚠ warn`（记账不阻断）/ `⊘ waived`（人已批准）/
`⏸ manual`（转人工，未批准前按阻断）/ `⛔ halted`（重试超限）/ `💥 error`（检查自身失败）

退出码：`0` pass、`1` blocked、`2` halted、`3` spec 错误、`4` 环境错误。

## 2. 接口契约

### 2.1 判定器接口（唯一扩展点）

```python
# scripts/gate/checks/check_*.py
def run(ctx: Context, rule: Rule) -> list[Finding]:
    """只读判定；阈值一律从 rule 读取；不得抛未捕获异常。"""
```

```python
# scripts/gate/model.py
@dataclass
class Finding:
    rule: str; status: Status; subject: str; message: str
    observed: Any; threshold: Any
    level: str; evidence: str | None; hint: str | None; line: int | None
```
`observed/threshold` 必填：**人类不需要回看日志就能复核**，这是"可审计"的最小要求。

### 2.2 事实采集接口

```python
# scripts/gate/context.py
Context.git: GitFacts        # worktree 相对 HEAD，或 CI 的 A..B 区间
Context.affected()           # 剔除 build/third_party 等后的改动文件
Context.read(rel) / sha256(rel)
Context.run(cmd, timeout)    # 统一超时，避免判定器各自裸调子进程
```
一次采集、多规则只读共享 → 同一份事实只测一次，结果可复现。

### 2.3 规则规格（唯一真相源）

`gate/spec.toml`：`[meta]`（重试上限、审批人、受保护路径、忽略集）+ `[rules.<ID>]`
（title/mode/check/params）。阈值全部在这里，代码内不得硬编码。

## 3. 数据结构与数据流转

```
git diff ─┐
文档文本  ├─► Context（一次采集）─► 16 条 check（并发只读）─► Finding[]
编译数据库┘                                                      │
                                                                 ▼
   GateReport ◄── runner 汇总（应用 waiver、判定 verdict、attempts+1）
       │
       ├─► local/gate/<run_id>/report.json（机器）＋ report.md（人类）＋ evidence/
       ├─► gate/ledger.jsonl（审计，append-only）
       └─► gate/tasks/<id>.json（状态与尝试次数）
```

| 产物 | 性质 | 是否入库 | 保留策略 |
|---|---|---|---|
| `gate/spec.toml` | 规则 | ✅ | 受保护，改动需审批 |
| `gate/tasks/<id>.json` | 任务契约 | ✅ | 任务结束归档 |
| `gate/waivers.jsonl` | 例外账本 | ✅ | append-only，永不删 |
| `gate/ledger.jsonl` | 审计账本 | ✅ | append-only，永不删 |
| `gate/baseline.json` | 存量棘轮 | ✅ | 只允许收紧（数值下降） |
| `gate/protected_tests.json` | 测试资产哈希 | ✅ | 更新需审批 |
| `local/gate/<run>/**` | 运行产物 | ❌（gitignore） | 保留最近若干次 |

### 关于"数据库"的结论：**不引入 RDBMS**

理由：本系统无服务端、无并发写、无在线查询；唯一硬需求是"不可篡改的追加账本"。
JSONL 天然满足（文本可 diff、可审查、可 hash 链），而引入 SQLite 属于给人添维护面。
**预留（当前不实现）**：若日后需要按规则聚合统计，`gate/index.db` 作为
**可重建投影**（drop 后由 JSONL 全量重建），绝不作为唯一真相源。

## 4. 关键设计决策与取舍

| 决策 | 取舍 | 理由 |
|---|---|---|
| 退出码而非异常 | 少表达力 | CI/本地/hook 统一消费，避免各自解析文本 |
| fail-closed（未批准的例外=阻断） | 会挡住"好意" | 放行未批准例外等于门禁无效 |
| 目标集用 compile_commands.json | 依赖构建产物存在 | 避免"门禁自己去猜编译参数"造成跨平台误报 |
| 质量类规则用**棘轮**而非零容忍 | 存量问题不一次性清零 | 零容忍会让门禁永久红→必然被关掉 |
| `TEST-001` 转人工而非直接阻断 | 降低自动化程度 | "行为变更是否合理"是判断，机器不该假装能判 |
| 负向夹具（该红必须红） | 每条规则多一份夹具成本 | 否则无法区分"规则有效"与"规则永远绿" |

## 5. 最坏情况与处置

| 最坏情况 | 处置 |
|---|---|
| 门禁永远红 → 被集体忽略 | 棘轮 + 任务契约 + 例外流程：红必须"有名字、有人负责" |
| 规则形同虚设（永远绿） | `selftest` 16 条负向夹具 + `--strict`（CI 零 SKIP） |
| 缺工具导致"未验证却当通过" | 环境不满足一律 WARN/ERROR/SKIP 并显式标注，**绝不静默通过** |
| 代理无限重试 | `max_gate_attempts=3` → `HALTED`(exit 2) + CI concurrency 取消旧运行 |
| 判定器卡死 | 判定器内部用 `ctx.run(timeout=…)`；runner 把异常转 ERROR（线程不可强杀，已在 README 声明） |
| 证据散落/丢失 | 证据统一写 `local/gate/<run_id>/evidence/`，报告索引引用 |
| 基线被顺手放宽 | `baseline.json` 在受保护路径内，放宽需非作者审批 |
| 多平台共用工作区导致误判 | compile_commands 按 `directory` 是否本机存在做平台匹配 |

## 6. 与项目交付纪律的接口

1. 每个提交 500–800 行、逻辑完整可运行；先过最小测试 → 相关 ctest → `gate run` → 本地提交。
2. **提交后不推送**；阶段性提交由用户复核/修订，用户确认后才推送。
3. 审计命名：`[audit-xX] + 英文类型(scope): 说明`。
4. 阶段收尾：更新 `notes.txt` / `diff.md` / 预算内文档，再进入下一阶段。

## 7. 落地性复检（"到底能不能实现"）

| 检查项 | 结论 | 证据 |
|---|---|---|
| 16 条规则是否都有实现 | ✅ | `Spec.check_registry_consistency` 在每次运行前双向校验 |
| 规则是否"真的会红" | ✅ | `selftest` 17/17（16 规则 + 干净场景不误报） |
| 是否能在两种 OS 跑 | ✅ | Windows py3.13 与 WSL py3.12 均实跑；账本并发追加 8 进程×10 次无丢失 |
| 退出码是否被 CI 消费 | ✅ | `.github/workflows/gate.yml` 的 gate job 按退出码判定并上传报告 |
| 阈值是否可调而不改代码 | ✅ | 全在 `gate/spec.toml`；规则变更走 CI-001 |
| 是否会被"顺手关掉" | ✅ | 受保护路径 + CODEOWNERS + waiver 需非作者 |
| 存量问题会不会卡死上线 | ✅ | `gate/baseline.json` 棘轮（字数/警告/复杂度/cppcheck） |
| 已知边界（诚实声明） | ⚠ | ①本地无法强制"批准者≠作者"（只有 GitHub 分支保护能强制）；②无判定器级超时（线程不可强杀）；③Windows 只覆盖可移植核心（SPEC 第 2 节） |
