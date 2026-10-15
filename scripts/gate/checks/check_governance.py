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
def rule_guard(ctx: Context, rule: Rule) -> list[Finding]:
    """受保护路径变更必须附带"非作者"批准的有效 waiver。"""
    findings: list[Finding] = []
    protected = ctx.protected_changed()
    if not protected:
        return [
            Finding(
                rule=rule.id, status=Status.PASS, subject="(受保护路径)",
                message=f"本次改动未触碰受保护路径（共 {len(ctx.spec.protected_paths)} 条规则）",
                observed=0, threshold=0, level="info",
            )
        ]

    author = (ctx.git.author_name or "").strip()
    require_waiver = bool(rule.get("require_waiver", True))
    require_distinct = bool(rule.get("require_distinct_actor", True))
    max_age_days = int(rule.get("waiver_max_age_days", 30))

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
            rule=rule.id, status=Status.BLOCK, subject=",".join(protected),
            message=(
                f"受保护路径变更缺少有效批准：{'；'.join(detail)}"
                f"（变更作者 {author or '(未知)'}）"
            ),
            observed=f"有效 waiver 0 个（候选 {len(rejects)} 个）",
            threshold=(
                f">=1 个 waiver 覆盖这些文件，approved_by 属于 {ctx.spec.rule_approvers} 且 != 变更作者"
            ),
            level="error",
            hint=(
                "申请例外：`python scripts/gate/cli.py exception request --rule CI-001 "
                f"--scope {protected[0]}`，由非作者审批人批准后重跑门禁。"
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
