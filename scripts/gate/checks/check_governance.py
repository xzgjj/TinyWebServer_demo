"""F 族：治理纪律（CI-001 规则/受保护文件变更 / GOV-001 重试轮数 / GOV-002 审计留痕）。

关键立场：**任何"依赖模块还没写好"的情况都必须显式阻断，不能静默通过**。
  * waivers 账本缺失 -> BLOCK "例外账本模块缺失"；
  * ledger 校验模块缺失 -> BLOCK "审计账本模块缺失"。
这样"另一个代理还没交付"只会让门禁变红并说清原因，而不是让门禁变成绿灯摆设。
"""
from __future__ import annotations

from datetime import datetime, timedelta, timezone
from typing import Any

from ..context import Context, match_any
from ..model import Finding, Status
from ..spec import Rule

CHECKS = ("rule_guard", "retry_cap", "audit")


# --------------------------------------------------------------------------- #
# CI-001 规则文件变更须指定人批准
# --------------------------------------------------------------------------- #
def _author_identities(ctx: Context) -> list[str]:
    """被审查改动集的全部作者身份（"名字 <邮箱>" 形式）；退化时用 git config。"""
    facts = ctx.git
    authors = list(getattr(facts, "authors", []) or [])
    if not authors and facts.author_name:
        authors = [facts.author_name]
    return [str(a) for a in authors]


def rule_guard(ctx: Context, rule: Rule) -> list[Finding]:
    """受保护路径变更必须附带"非作者"批准的有效 waiver。

    **自举语义（引入即基线）**：只对"基线里已存在的受保护文件被修改/删除/重命名"要求审批；
    "新增受保护文件"视为**建立基线**，只记录、不阻断。
    为什么必须这样：规则体系是由"被它自己保护的这段改动"引入的，
    此时唯一审批人就是写作者本人 -> "非作者批准"在逻辑上无解（实测死锁，见 audit-19/23）。
    防绕过：删除(D)与重命名(R)仍按"改动"处理（否则删掉再重建即可清空保护）。
    """
    findings: list[Finding] = []
    protected = ctx.protected_modified()      # 需要审批的部分：已存在文件被改/删/改名
    baseline_new = ctx.protected_new()        # 建立基线：新增的保护文件
    if not protected and not baseline_new:
        return [
            Finding(
                rule=rule.id, status=Status.PASS, subject="(受保护路径)",
                message=f"本次改动未触碰受保护路径（共 {len(ctx.spec.protected_paths)} 条规则）",
                observed=0, threshold=0, level="info",
            )
        ]
    if not protected:
        return [
            Finding(
                rule=rule.id, status=Status.PASS, subject=",".join(baseline_new[:5]) + (
                    f" 等 {len(baseline_new)} 个文件" if len(baseline_new) > 5 else ""),
                message=(
                    f"建立基线：新增受保护文件 {len(baseline_new)} 个，本次不需要非作者审批；"
                    "从下次修改起（修改/删除/重命名）才需要审批"
                ),
                observed=len(baseline_new), threshold=0, level="info",
                hint="新增保护文件属「引入即基线」：请人类在合并前确认这些文件确实应当受保护。",
            )
        ]
    findings.append(
        Finding(
            rule=rule.id, status=Status.PASS, subject="(建立基线)",
            message=(
                f"其中 {len(baseline_new)} 个受保护文件为新增（建立基线，不需审批）；"
                f"下面只审 {len(protected)} 个已存在文件的改动"
            ) if baseline_new else f"需审批的受保护文件改动：{len(protected)} 个",
            observed=len(baseline_new), threshold=0, level="info",
        )
    )

    author = (ctx.git.author_name or "").strip()
    require_waiver = bool(rule.get("require_waiver", True))
    require_distinct = bool(rule.get("require_distinct_actor", True))
    max_age_days = int(rule.get("waiver_max_age_days", 30))
    bootstrap = ctx.bootstrap_mode()

    try:
        from ..waivers import active_waivers      # type: ignore[attr-defined]
    except ImportError as exc:
        return [
            Finding(
                rule=rule.id, status=Status.BLOCK, subject="(例外账本)",
                message=(
                    f"例外账本模块缺失（scripts/gate/waivers.py 尚未提供）："
                    f"无法验证受保护路径变更 {protected} 是否已获批"
                ),
                observed=f"ImportError: {exc}", threshold="active_waivers() 可用",
                level="error",
                hint=(
                    "补齐 scripts/gate/waivers.py（active_waivers(root, spec, commit, author)）；"
                    "在补齐之前，受保护路径的变更一律阻断。"
                ),
            )
        ]

    try:
        waivers = list(active_waivers(ctx.root, ctx.spec, ctx.git.commit, author))
    except Exception as exc:  # noqa: BLE001 - 账本坏了必须显式失败
        return [
            Finding(
                rule=rule.id, status=Status.BLOCK, subject="(例外账本)",
                message=f"读取例外账本失败: {type(exc).__name__}: {exc}",
                observed=type(exc).__name__, threshold="可读取的例外账本", level="error",
            )
        ]

    rejects: list[tuple[str, str]] = []          # (waiver_id, 拒绝原因)
    for waiver in waivers:
        wid = str(getattr(waiver, "waiver_id", "?"))
        approved_by = str(getattr(waiver, "approved_by", "") or "").strip()
        scope_files = list(getattr(waiver, "scope_files", []) or [])
        covered = [p for p in protected if match_any(p, scope_files)]
        if not covered:
            continue
        if not approved_by:
            rejects.append((wid, "缺少 approved_by"))
            continue
        if require_distinct and approved_by.lower() == "agent":
            rejects.append((wid, "approved_by=agent 不是人类审批人"))
            continue
        if require_distinct and author and approved_by == author:
            rejects.append((wid, f"approved_by 与变更作者相同（{approved_by}）"))
            continue
        if require_distinct:
            # 被审查的改动集可能由多个身份提交（人 + 代理）：只要批准人写过其中任意一个
            # 提交，就属于自批。只看 git config 会被"改一下 user.name"绕过（实测可行）。
            offenders = [a for a in _author_identities(ctx) if approved_by in a]
            if offenders:
                rejects.append((wid, f"approved_by 是该改动集的作者之一（{offenders[0]}）"))
                continue
        expiry = _parse_dt(getattr(waiver, "expires_at", None))
        now = datetime.now(timezone.utc)
        if expiry is not None:
            if expiry < now:
                rejects.append((wid, f"waiver 已过期（{expiry.date()}）"))
                continue
            if now < expiry - timedelta(days=max_age_days):
                rejects.append(
                    (wid, f"签发时间早于 waiver_max_age_days={max_age_days} 的有效窗口")
                )
                continue
        if bool(getattr(waiver, "expired", False)):
            rejects.append((wid, "waiver 被标记为已过期"))
            continue
        if require_waiver:
            return [
                Finding(
                    rule=rule.id, status=Status.PASS, subject=",".join(protected),
                    message=(
                        f"受保护路径变更（{len(protected)} 个文件）已由 {approved_by} 批准："
                        f"waiver {wid} 覆盖 {len(covered)} 个文件"
                    ),
                    observed=approved_by, threshold=f"!= {author or '(变更作者)'}", level="info",
                )
            ]
        findings.append(
            Finding(
                rule=rule.id, status=Status.WARN, subject=",".join(protected),
                message=(
                    f"受保护路径变更存在 waiver {wid}（{approved_by}），但 require_waiver=false，"
                    "本次未强制要求批准"
                ),
                observed=approved_by, threshold="require_waiver=false", level="warning",
            )
        )
        return findings

    detail = [f"{wid}: {why}" for wid, why in rejects] or ["没有任何 waiver 覆盖这些文件"]
    if not require_waiver:
        findings.append(
            Finding(
                rule=rule.id, status=Status.WARN, subject=",".join(protected),
                message=(
                    f"受保护路径变更没有有效批准，但 require_waiver=false（{'；'.join(detail)}）"
                ),
                observed=None, threshold="require_waiver=false", level="warning",
            )
        )
        return findings

    findings.append(
        Finding(
            rule=rule.id,
            status=Status.WARN if bootstrap else Status.BLOCK,
            subject=",".join(protected),
            message=(
                (
                    f"[自举期] 门禁体系首次引入（规则与引擎在基线里都不存在）："
                    f"{len(protected)} 个受保护文件的改动降级为待人工确认，不阻断。"
                    f"请人类在合并前确认这些改动应当被接受；合并后本规则自动恢复阻断。"
                    f"（未获批准的原因：{'；'.join(detail)}）"
                )
                if bootstrap
                else (
                    f"受保护路径变更缺少有效批准：{'；'.join(detail)}"
                    f"（变更作者 {author or '(未知)'}）"
                )
            ),
            observed=f"有效 waiver 0 个（候选 {len(rejects)} 个）",
            threshold=(
                "自举期：人类合并确认即可（WARN）"
                if bootstrap
                else f">=1 个 waiver 覆盖这些文件，approved_by 属于 {ctx.spec.rule_approvers} 且 != 变更作者"
            ),
            level="warning" if bootstrap else "error",
            hint=(
                "自举期说明：门禁的第一版只能由被它自己保护的改动引入，此时"
                "「非作者批准」在逻辑上无解（唯一审批人即写作者）。因此自举期只记账不阻断；"
                "一旦规则进入基线分支，任何后续修改都会恢复为阻断。"
                if bootstrap
                else (
                    "申请例外：`python scripts/gate/cli.py exception request --rule CI-001 "
                    f"--base <base>..HEAD --by <申请人>`，由非作者审批人批准后重跑门禁。"
                )
            ),
        )
    )
    return findings


def _parse_dt(value: Any) -> datetime | None:
    """解析 waiver 的 expires_at（容忍 str/datetime、带不带时区）。"""
    if isinstance(value, datetime):
        return value if value.tzinfo else value.replace(tzinfo=timezone.utc)
    if not isinstance(value, str) or not value.strip():
        return None
    text = value.strip().replace("Z", "+00:00")
    for fmt in ("%Y-%m-%dT%H:%M:%S%z", "%Y-%m-%dT%H:%M:%S", "%Y-%m-%d"):
        try:
            dt = datetime.strptime(text, fmt)
        except ValueError:
            continue
        return dt if dt.tzinfo else dt.replace(tzinfo=timezone.utc)
    try:
        dt = datetime.fromisoformat(text)
    except ValueError:
        return None
    return dt if dt.tzinfo else dt.replace(tzinfo=timezone.utc)


# --------------------------------------------------------------------------- #
# GOV-001 门禁重试轮数上限
# --------------------------------------------------------------------------- #
def retry_cap(ctx: Context, rule: Rule) -> list[Finding]:
    """同一任务连续失败达到上限 -> HALTED（禁止无限自动重试）。"""
    max_attempts = int(rule.get("max_attempts", ctx.spec.max_gate_attempts))
    if ctx.task is None:
        return [
            Finding(
                rule=rule.id, status=Status.PASS, subject="(任务契约)",
                message="没有任务契约，重试上限无从谈起（契约缺失由 SCOPE-001 判定）",
                observed=None, threshold=max_attempts, level="info",
            )
        ]
    attempts = int(ctx.task.attempts)
    reset_hint = (
        f'需人工重置：python scripts/gate/cli.py reset --task {ctx.task.id} --reason "..."'
    )
    if attempts >= max_attempts:
        return [
            Finding(
                rule=rule.id, status=Status.HALTED, subject=ctx.task.id,
                message=(
                    f"任务 {ctx.task.id} 已连续失败 {attempts} 次，达到上限 {max_attempts}："
                    f"停止自动重试。{reset_hint}"
                ),
                observed=attempts, threshold=max_attempts, level="error",
                hint=reset_hint,
            )
        ]
    return [
        Finding(
            rule=rule.id, status=Status.PASS, subject=ctx.task.id,
            message=f"任务 {ctx.task.id} 已尝试 {attempts}/{max_attempts} 次，未达上限",
            observed=attempts, threshold=max_attempts, level="info",
        )
    ]


# --------------------------------------------------------------------------- #
# GOV-002 审计留痕
# --------------------------------------------------------------------------- #
def audit(ctx: Context, rule: Rule) -> list[Finding]:
    """校验 ledger 必需字段；账本模块缺失即阻断。"""
    required = [str(x) for x in (rule.get("required_fields") or [])]
    ledger_rel = str(ctx.spec.meta.get("ledger_path", "gate/ledger.jsonl"))
    try:
        from ..ledger import verify            # type: ignore[attr-defined]
    except ImportError as exc:
        return [
            Finding(
                rule=rule.id, status=Status.BLOCK, subject="(审计账本)",
                message=(
                    "审计账本模块缺失（scripts/gate/ledger.py 尚未提供）："
                    f"无法校验 {ledger_rel} 的必需字段 {required}"
                ),
                observed=f"ImportError: {exc}", threshold="verify(root, spec) 可用",
                level="error",
                hint=(
                    "补齐 scripts/gate/ledger.py（verify(root, spec) -> list[str]）；"
                    "缺失期间审计留痕一律阻断。"
                ),
            )
        ]

    try:
        problems = list(verify(ctx.root, ctx.spec))
    except FileNotFoundError as exc:
        return [
            Finding(
                rule=rule.id, status=Status.BLOCK, subject=ledger_rel,
                message=f"审计账本文件缺失: {exc}",
                observed="missing", threshold="存在且字段完整", level="error",
                hint="账本由 runner 在每次运行后追加；若从未生成，说明审计留痕从未生效。",
            )
        ]
    except Exception as exc:  # noqa: BLE001 - 账本坏了必须显式失败
        return [
            Finding(
                rule=rule.id, status=Status.BLOCK, subject=ledger_rel,
                message=f"账本校验异常: {type(exc).__name__}: {exc}",
                observed=type(exc).__name__, threshold="verify() 正常返回", level="error",
            )
        ]

    if problems:
        return [
            Finding(
                rule=rule.id, status=Status.BLOCK, subject=ledger_rel,
                message=f"账本存在 {len(problems)} 个问题: {problems[:10]}",
                observed=len(problems), threshold=0, level="error",
                hint=f"补齐必需字段: {required}（每条记录都要完整）。",
            )
        ]
    return [
        Finding(
            rule=rule.id, status=Status.PASS, subject=ledger_rel,
            message=f"账本必需字段校验通过（{len(required)} 个字段，账本模块可用）",
            observed=0, threshold=0, level="info",
        )
    ]
