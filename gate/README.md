# CI 门禁引擎（gate）

本目录是"改动能不能进主干"的唯一判定入口。规则写在 [`spec.toml`](spec.toml)（唯一真相源），
引擎在 `scripts/gate/`（纯标准库，Python ≥ 3.11，Windows / WSL 通用）。

## 它解决什么

失败模式不是"没有检查"，而是"检查看起来在跑，其实永远绿"。所以引擎强制三件事：

1. **规则必须落地**：spec 声明的 check 与代码注册表双向校验（缺实现 / 未引用都报错 → 退出码 3）。
2. **检查崩了不许静默**：check 抛异常、返回类型不对，一律记 `ERROR` 并按阻断处理。
3. **越轨必须留痕**：任何例外都要有申请、理由、**非作者**批准、到期日，并写进账本。

## 用法

```bash
python scripts/gate/cli.py declare --task T-001 --scope "src/**,include/**" --budget 800
python scripts/gate/cli.py run --task T-001                 # 并发执行（默认 4 线程）
python scripts/gate/cli.py run --task T-001 --serial        # 串行（复现问题用）
python scripts/gate/cli.py run --task T-001 --skip-heavy    # 跳过需要编译的重规则（标 warn）
python scripts/gate/cli.py run --task T-001 --json          # 只输出机器可读 JSON
python scripts/gate/cli.py run --base main..HEAD            # CI 模式：只判定区间
python scripts/gate/cli.py status | explain SCOPE-002 | report --run last
python scripts/gate/cli.py exception request --rule DOC-001 --reason "为什么必须越轨"
python scripts/gate/cli.py exception approve --id W-xxxx --by xzgjj
python scripts/gate/cli.py reset --task T-001 --reason "人工确认后可再试"
python scripts/gate/cli.py selftest            # 负向 fixture 自检（由 checks 侧提供）
```

`--root PATH` 可指定仓库根（默认＝本脚本所在仓库根），放在子命令前后都行。

## 退出码（model.ExitCode，CI 直接消费）

| 码 | 含义 | 触发条件 |
| --- | --- | --- |
| 0 | pass | 只剩 WARN / WAIVED（warn 明细仍保留在报告里） |
| 1 | blocked | 存在非 waived 的 BLOCK / MANUAL / ERROR |
| 2 | halted | 有规则返回 HALTED，或连续失败次数超过 `max_gate_attempts`；只能人工 `reset` |
| 3 | spec 错误 | `spec.toml` 不合法、check 未实现/未被引用、引用未定义规则 |
| 4 | 环境错误 | 任务契约损坏、IO 失败、非法操作（如自批例外）、未预期异常 |

## 判定语义（机器规则，写死在引擎里）

- 每条 finding 由 check 给出 `status`；规则状态取其 findings 的最严者（`RuleResult.status`）。
- 汇总：任一非 waived 的 BLOCK/MANUAL/ERROR → `blocked`；HALTED → `halted`；只有 WARN → `pass`。
- **例外只覆盖 BLOCK / MANUAL**：ERROR（检查自身出错）与 HALTED（重试超限）不可被例外掩盖。
- 尝试计数：`attempts` = 连续失败次数，PASS 即归零；判定式为 `attempts > max_gate_attempts`。
  以 spec 的 `max_gate_attempts = 3` 为例：**允许失败 3 次，第 4 次运行仍失败即 HALTED**。
- 并发执行但**报告顺序永远等于 spec 声明顺序**（`--serial` 关闭并发，用于复现）。

## 账本与例外（都是 append-only）

- `gate/ledger.jsonl`：每次运行一行（`run`），治理动作（`reset`）也写这里。
  字段含 GOV-002 的 `required_fields`（从 spec 读取）+ `branch/attempt/counts/blocking_rules/waived_rules/warnings`。
  `ledger.verify()` 逐行校验必需字段，由 GOV-002 的 `audit` 检查调用。
- `gate/waivers.jsonl`：例外事件流（`request`/`approve`/`reject`），当前状态靠回放得出，历史不可抹。
  一条例外**真正生效**需同时满足：

  | 条件 | 说明 |
  | --- | --- |
  | `status = approved` | 未批准 / 已驳回 = 不生效 |
  | 未过期 | `effective_expiry = min(expires_at, created_at + 规则声明的 waiver_max_age_days)` |
  | `rule` 仍存在于 spec | 规则被删则例外失效 |
  | `approved_by` ≠ 变更作者 且 ≠ 申请人 | CI-001 职责分离 |
  | `commit` 为空或与当前 HEAD 前缀匹配 | 空 = 不绑定提交 |
  | `scope_files` 为空或与本次改动文件有交集 | 空 = 不限范围 |

- 两个账本都用"读旧 + 写临时文件 + `os.replace`"原子追加，并用独占锁文件串行化并发写入；
  中断或并发都不会写出半行损坏的账本。
- `exception reject` 允许申请人撤回自己的申请（只会让门禁更严）；其他人驳回必须是 `rule_approvers` 成员。

## 产物

```
local/gate/<run_id>/report.json    # 完整快照，json.loads 可直接解析
local/gate/<run_id>/report.md      # 人类可读：1 汇总 2 阻断 3 例外 4 警告 5 转人工 6 证据与下一步
local/gate/<run_id>/evidence/      # 证据索引（只记原始路径，不复制大文件）
```

`report.md` 刻意把**机器判定**（规则 / observed / threshold / hint 原样输出）与
**人的判断**（已批准的例外、转人工项）分成不同段落：机器只摆事实，越轨由人签字。

## 已知边界

- 线程不可强杀：check 卡死时引擎无法强制中断，超时必须由 check 自己用 `ctx.run(timeout=...)` 兜。
- `--skip-heavy` 跳过的规则记为 WARN 并写明 `SKIPPED`，**不等于通过**，请在全量 CI 补跑。
- git 事实不可用时（非 git 工作区）引擎不失败，但会在报告里标注"改动集为空，SCOPE/预算类结论不可信"。
