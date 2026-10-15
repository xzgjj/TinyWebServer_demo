"""例外账本（gate/waivers.jsonl）：把"人的判断"变成可审计、可过期的一等对象。

为什么存在：门禁不允许"顺手关规则"，任何越轨都要留下申请人、**非作者**批准人、
理由、到期日与影响范围；账本 append-only，当前状态靠回放事件得出，历史抹不掉。
安全底线一律"失败即更严"：未批准/已过期/自批/范围不匹配/时间不可解析都算不生效。
"""
from __future__ import annotations

import json
import uuid
from dataclasses import dataclass, field
from datetime import datetime, timedelta, timezone
from pathlib import Path
from typing import Any, Iterable

from .context import collect_git, match_any
from .ledger import atomic_append_line
from .spec import Spec, SpecError
from .task import now_iso

STATUS_PENDING = "pending"
STATUS_APPROVED = "approved"
STATUS_REJECTED = "rejected"

DEFAULT_TTL_DAYS = 14
_EPOCH = datetime(1970, 1, 1, tzinfo=timezone.utc)


class WaiverError(Exception):
    """例外流程被违反（自我批准、批准人非法、例外不存在等）。"""


# --------------------------------------------------------------------------- #
# 数据模型
# --------------------------------------------------------------------------- #
@dataclass
class Waiver:
    """一条例外的"当前状态"（由账本回放得到，不是独立存储）。"""

    waiver_id: str
    rule: str
    task: str = ""
    commit: str = ""
    requested_by: str = ""
    approved_by: str = ""
    reason: str = ""
    created_at: str = ""
    expires_at: str = ""
    scope_files: list[str] = field(default_factory=list)
    # —— 回放派生字段 ——
    status: str = STATUS_PENDING
    decided_at: str = ""
    rejected_by: str = ""
    reject_reason: str = ""

    def to_dict(self) -> dict[str, Any]:
        return {
            "waiver_id": self.waiver_id,
            "rule": self.rule,
            "task": self.task,
            "commit": self.commit,
            "requested_by": self.requested_by,
            "approved_by": self.approved_by,
            "reason": self.reason,
            "created_at": self.created_at,
            "expires_at": self.expires_at,
            "scope_files": list(self.scope_files),
            "status": self.status,
            "decided_at": self.decided_at,
            "rejected_by": self.rejected_by,
            "reject_reason": self.reject_reason,
        }

    @classmethod
    def from_dict(cls, data: dict[str, Any]) -> "Waiver":
        known = {f for f in cls.__dataclass_fields__}  # type: ignore[attr-defined]
        obj = cls(**{k: v for k, v in data.items() if k in known and v is not None})
        if not isinstance(obj.scope_files, list):
            obj.scope_files = [str(obj.scope_files)]
        obj.scope_files = [str(s) for s in obj.scope_files]
        return obj

    @property
    def is_approved(self) -> bool:
        return self.status == STATUS_APPROVED and bool(self.approved_by)


# --------------------------------------------------------------------------- #
# 时间与规格辅助
# --------------------------------------------------------------------------- #
def _parse_dt(text: str) -> datetime | None:
    s = (text or "").strip()
    if not s:
        return None
    for fmt in ("%Y-%m-%dT%H:%M:%S%z", "%Y-%m-%dT%H:%M:%S", "%Y-%m-%d %H:%M:%S", "%Y-%m-%d"):
        try:
            dt = datetime.strptime(s, fmt)
        except ValueError:
            continue
        return dt.astimezone(timezone.utc)  # 朴素时间按本机时区解释后再统一到 UTC
    return None


def load_spec(root: str | Path) -> Spec | None:
    """尝试加载仓库规格；失败返回 None（调用方决定是拒绝还是降级）。"""
    p = Path(root) / "gate" / "spec.toml"
    if not p.is_file():
        return None
    try:
        return Spec.load(p)
    except SpecError:
        return None


def waiver_path(root: str | Path, spec: Spec | None = None) -> Path:
    rel = spec.path_of("waiver_path", "gate/waivers.jsonl") if spec else Path("gate/waivers.jsonl")
    p = Path(rel)
    return p if p.is_absolute() else Path(root) / p


def _require_spec(root: str | Path, spec: Spec | None) -> Spec:
    s = spec or load_spec(root)
    if s is None:
        raise WaiverError(f"找不到或无法解析规格文件: {Path(root) / 'gate' / 'spec.toml'}（例外必须依据规格裁定）")
    return s


# --------------------------------------------------------------------------- #
# 读取：回放账本
# --------------------------------------------------------------------------- #
def _records(root: str | Path, spec: Spec | None = None) -> list[dict[str, Any]]:
    p = waiver_path(root, spec)
    if not p.is_file():
        return []
    out: list[dict[str, Any]] = []
    for line in p.read_text(encoding="utf-8", errors="replace").splitlines():
        s = line.strip()
        if not s:
            continue
        try:
            rec = json.loads(s)
        except json.JSONDecodeError:
            continue  # 损坏行不影响其余记录；账本完整性由 CI-001 侧的检查关注
        if isinstance(rec, dict) and rec.get("waiver_id"):
            out.append(rec)
    return out


def list_all(root: str | Path, spec: Spec | None = None) -> list[Waiver]:
    """回放全部事件，返回每条例外的当前状态（按首次出现顺序）。"""
    order: list[str] = []
    merged: dict[str, Waiver] = {}
    for rec in _records(root, spec):
        wid = str(rec.get("waiver_id"))
        event = str(rec.get("event") or "request")
        w = merged.get(wid)
        if w is None:
            w = Waiver.from_dict(rec)
            merged[wid] = w
            order.append(wid)
        if event == "request":
            w.status = str(rec.get("status") or STATUS_PENDING)
        elif event == "approve":
            w.approved_by = str(rec.get("approved_by") or "")
            w.decided_at = str(rec.get("decided_at") or "")
            w.rejected_by = ""
            w.reject_reason = ""
            w.status = STATUS_APPROVED if w.approved_by else STATUS_PENDING
        elif event == "reject":
            w.rejected_by = str(rec.get("rejected_by") or "")
            w.reject_reason = str(rec.get("reject_reason") or "")
            w.decided_at = str(rec.get("decided_at") or "")
            w.approved_by = ""            # 驳回即撤销既有批准
            w.status = STATUS_REJECTED
    return [merged[i] for i in order]


def find(root: str | Path, waiver_id: str, spec: Spec | None = None) -> Waiver | None:
    for w in list_all(root, spec):
        if w.waiver_id == waiver_id:
            return w
    return None


def pending(root: str | Path, spec: Spec | None = None) -> list[Waiver]:
    return [w for w in list_all(root, spec) if w.status == STATUS_PENDING]


# --------------------------------------------------------------------------- #
# 生效判定
# --------------------------------------------------------------------------- #
def effective_expiry(w: Waiver, spec: Spec | None) -> datetime | None:
    """有效到期时刻 = min(显式 expires_at, created_at + 规则声明的最大年龄)。

    规则侧上限取自该规则 params 里的 waiver_max_age_days（如 CI-001=30 天），
    阈值一律从 spec 读取，代码里不写死。
    """
    caps: list[datetime] = []
    if w.expires_at:
        dt = _parse_dt(w.expires_at)
        if dt is None:
            return _EPOCH  # 时间不可解析 -> 视为已过期（fail closed）
        caps.append(dt)
    if spec is not None:
        try:
            max_age = spec.rule(w.rule).get("waiver_max_age_days")
        except SpecError:
            max_age = None
        if max_age:
            created = _parse_dt(w.created_at)
            if created:
                caps.append(created + timedelta(days=int(max_age)))
    return min(caps) if caps else None


def is_expired(w: Waiver, spec: Spec | None) -> bool:
    exp = effective_expiry(w, spec)
    return exp is not None and datetime.now(timezone.utc) >= exp


def _commit_matches(waiver_commit: str, commit: str) -> bool:
    a = (waiver_commit or "").strip().lower()
    b = (commit or "").strip().lower()
    if not a:
        return True                      # 空 = 不绑定具体提交
    if not b:
        return False                     # 无法确定当前提交 -> 不生效（fail closed）
    return a == b or a.startswith(b) or b.startswith(a)


def _scope_hits(scope_files: Iterable[str], changed: Iterable[str]) -> bool:
    pats = [str(p).replace("\\", "/") for p in scope_files if str(p).strip()]
    if not pats:
        return True                      # 空 = 不限定文件范围
    return any(match_any(str(p).replace("\\", "/"), pats) for p in changed)


def active_waivers(
    root: str | Path,
    spec: Spec,
    commit: str,
    author: str,
    affected: Iterable[str] | None = None,
) -> list[Waiver]:
    """返回当前真正生效的例外（已批准 + 未过期 + 规则仍在 + 范围命中 + 非自批）。

    affected 为本次改动文件（仓库相对 POSIX 路径）；不传则现场采集 git 事实。
    """
    if affected is None:
        facts = collect_git(Path(root))
        changed = [p for p in facts.changed if not match_any(p, spec.exclude_globs)]
    else:
        changed = [str(p).replace("\\", "/") for p in affected]

    out: list[Waiver] = []
    for w in list_all(root, spec):
        if not w.is_approved:
            continue
        if w.rule not in spec.rules:            # 规则已被删除/改名 -> 例外失效
            continue
        if w.approved_by and author and w.approved_by == author:
            continue                            # 自批无效（CI-001 职责分离）
        if w.requested_by and w.requested_by == w.approved_by:
            continue                            # 双保险：申请人与批准人同一人
        if is_expired(w, spec):
            continue
        if not _commit_matches(w.commit, commit):
            continue
        if not _scope_hits(w.scope_files, changed):
            continue
        out.append(w)
    return out


# --------------------------------------------------------------------------- #
# 写入：申请 / 批准 / 驳回
# --------------------------------------------------------------------------- #
def _new_id(root: str | Path, spec: Spec | None) -> str:
    existing = {w.waiver_id for w in list_all(root, spec)}
    stamp = datetime.now().strftime("%y%m%d")
    for _ in range(20):
        wid = f"W-{stamp}-{uuid.uuid4().hex[:4]}"
        if wid not in existing:
            return wid
    raise WaiverError("无法生成唯一的 waiver_id（随机冲突过多）")


def _default_ttl(rule: str, spec: Spec) -> int:
    for rid in (rule, "CI-001"):
        try:
            v = spec.rule(rid).get("waiver_max_age_days")
        except SpecError:
            v = None
        if v:
            return int(v)
    return DEFAULT_TTL_DAYS


def request(
    root: str | Path,
    rule: str,
    task: str,
    reason: str,
    requested_by: str,
    ttl_days: int | None = None,
    spec: Spec | None = None,
    commit: str | None = None,
    scope_files: Iterable[str] | None = None,
) -> Waiver:
    """申请一条例外（状态 pending，approved_by 为空 —— 未批准即不生效）。"""
    s = _require_spec(root, spec)
    if rule not in s.rules:
        raise WaiverError(f"未定义的规则: {rule}；可用规则: {', '.join(sorted(s.rules))}")
    if not str(reason).strip():
        raise WaiverError("必须给出申请理由（reason 不能为空）")
    if not str(requested_by).strip():
        raise WaiverError("必须给出申请人（requested_by 不能为空）")

    days = int(ttl_days) if ttl_days else _default_ttl(rule, s)
    if days <= 0:
        raise WaiverError(f"ttl_days 必须为正整数，当前为 {days}")

    created_dt = datetime.now(timezone.utc)
    created = created_dt.astimezone().strftime("%Y-%m-%dT%H:%M:%S%z")
    expires = (created_dt + timedelta(days=days)).astimezone().strftime("%Y-%m-%dT%H:%M:%S%z")
    if commit is None:
        commit = collect_git(Path(root)).commit

    w = Waiver(
        waiver_id=_new_id(root, s),
        rule=rule,
        task=str(task or ""),
        commit=str(commit or ""),
        requested_by=str(requested_by),
        approved_by="",
        reason=str(reason).strip(),
        created_at=created,
        expires_at=expires,
        scope_files=[str(p).replace("\\", "/") for p in (scope_files or [])],
        status=STATUS_PENDING,
    )
    atomic_append_line(waiver_path(root, s), {"event": "request", **w.to_dict()})
    return w


def _approver_guard(root: str | Path, w: Waiver, approver: str, spec: Spec | None) -> Spec:
    s = _require_spec(root, spec)
    who = str(approver or "").strip()
    if not who:
        raise WaiverError("必须给出批准人（approver 不能为空）")
    if who not in s.rule_approvers:
        raise WaiverError(
            f"批准人 {who} 不在 spec.meta.rule_approvers={s.rule_approvers} 中，无权批准例外"
        )
    if who == w.requested_by:
        raise WaiverError(f"批准者与申请者不能是同一人（{who}），违反 CI-001 职责分离")
    return s


def approve(root: str | Path, waiver_id: str, approver: str, spec: Spec | None = None) -> Waiver:
    """批准一条例外；自批与非法批准人一律拒绝。"""
    s = _require_spec(root, spec)
    w = find(root, waiver_id, s)
    if w is None:
        raise WaiverError(f"例外不存在: {waiver_id}")
    _approver_guard(root, w, approver, s)
    if w.status == STATUS_REJECTED:
        raise WaiverError(f"{waiver_id} 已被 {w.rejected_by or '他人'} 驳回，不能直接批准；请重新申请")
    if w.status == STATUS_APPROVED:
        if w.approved_by == str(approver).strip():
            return w                     # 幂等：同一个人重复批准不报错
        raise WaiverError(f"{waiver_id} 已由 {w.approved_by} 批准，不能由 {approver} 再次批准")

    atomic_append_line(
        waiver_path(root, s),
        {
            "event": "approve",
            "waiver_id": waiver_id,
            "rule": w.rule,
            "approved_by": str(approver).strip(),
            "requested_by": w.requested_by,
            "decided_at": now_iso(),
            "status": STATUS_APPROVED,
        },
    )
    updated = find(root, waiver_id, s)
    if updated is None:  # 理论上不可能；显式处理而不是静默返回旧对象
        raise WaiverError(f"{waiver_id} 批准后无法回读账本，请检查 {waiver_path(root, s).name} 可写性")
    return updated


def reject(
    root: str | Path,
    waiver_id: str,
    approver: str,
    reason: str,
    spec: Spec | None = None,
) -> Waiver:
    """驳回（或撤销）一条例外，追加一条 rejected 记录。

    安全方向：驳回只会让门禁更严，因此申请人撤回自己的申请是允许的
    （无需具备审批资格）；其他人驳回则必须是有资格的审批人。
    """
    s = _require_spec(root, spec)
    w = find(root, waiver_id, s)
    if w is None:
        raise WaiverError(f"例外不存在: {waiver_id}")
    who = str(approver or "").strip()
    if not who:
        raise WaiverError("必须给出驳回人（approver 不能为空）")
    if who != w.requested_by:            # 申请人撤回自己的申请 -> 放行
        _approver_guard(root, w, who, s)
    if not str(reason).strip():
        raise WaiverError("驳回必须给出理由（reason 不能为空）")

    atomic_append_line(
        waiver_path(root, s),
        {
            "event": "reject",
            "waiver_id": waiver_id,
            "rule": w.rule,
            "rejected_by": who,
            "reject_reason": str(reason).strip(),
            "decided_at": now_iso(),
            "status": STATUS_REJECTED,
        },
    )
    updated = find(root, waiver_id, s)
    if updated is None:
        raise WaiverError(f"{waiver_id} 驳回后无法回读账本，请检查 {waiver_path(root, s).name} 可写性")
    return updated


def describe(w: Waiver) -> str:
    """一行摘要，供 CLI/报告共用。"""
    who = w.approved_by or w.rejected_by or "（待批准）"
    return f"{w.waiver_id} [{w.status}] {w.rule} 申请人={w.requested_by} 处理人={who} 到期={w.expires_at or '-'}"
