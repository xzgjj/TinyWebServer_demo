"""审计账本（gate/ledger.jsonl）：append-only 的运行与治理留痕。

为什么存在：没有账本就无法回答"跑了几次、上次 verdict 是什么、谁 reset 过"，
GOV-001 重试上限与 GOV-002 审计留痕也就无从落地。
写入用"读旧 + 临时文件 + os.replace"并配独占锁，中断/并发都不会写坏账本。
"""
from __future__ import annotations

import json
import os
import time
from pathlib import Path
from typing import Any

from .model import GateReport, Status
from .spec import Spec, SpecError
from .task import now_iso

#: 万一 spec 缺少 GOV-002（规格被改坏）时的兜底字段集，避免写出"无字段账本"。
FALLBACK_REQUIRED_FIELDS = [
    "run_id",
    "task",
    "commit",
    "verdict",
    "started_at",
    "duration_s",
    "spec_hash",
]

_LOCK_STALE_S = 60.0        # 锁文件超过该秒数视为持有者已崩溃，可强制接管
_LOCK_TIMEOUT_S = 10.0      # 获取锁的最长等待时间


# --------------------------------------------------------------------------- #
# 路径与规格读取
# --------------------------------------------------------------------------- #
def ledger_path(root: str | Path, spec: Spec | None = None) -> Path:
    """账本绝对路径（相对路径取自 spec.meta.ledger_path，不硬编码）。"""
    rel = spec.path_of("ledger_path", "gate/ledger.jsonl") if spec else Path("gate/ledger.jsonl")
    p = Path(rel)
    return p if p.is_absolute() else Path(root) / p


def required_fields(spec: Spec | None) -> list[str]:
    """GOV-002 的 required_fields（规格缺失时退回内置兜底集）。"""
    if spec is None:
        return list(FALLBACK_REQUIRED_FIELDS)
    try:
        fields = spec.rule("GOV-002").get("required_fields")
    except SpecError:
        return list(FALLBACK_REQUIRED_FIELDS)
    if not isinstance(fields, (list, tuple)) or not fields:
        return list(FALLBACK_REQUIRED_FIELDS)
    return [str(f) for f in fields]


# --------------------------------------------------------------------------- #
# 原子追加原语（waivers.py 复用）
# --------------------------------------------------------------------------- #
class _FileLock:
    """基于 O_CREAT|O_EXCL 的跨进程互斥锁；崩溃残留的锁会被判定过期后接管。"""

    def __init__(self, path: Path, timeout: float = _LOCK_TIMEOUT_S) -> None:
        self.path = path
        self.timeout = timeout
        self.fd: int | None = None

    def __enter__(self) -> "_FileLock":
        deadline = time.monotonic() + self.timeout
        while True:
            if time.monotonic() >= deadline:
                raise TimeoutError(f"获取账本锁超时（{self.timeout:.0f}s）: {self.path}")
            try:
                self.fd = os.open(str(self.path), os.O_CREAT | os.O_EXCL | os.O_WRONLY)
                os.write(self.fd, f"{os.getpid()} {now_iso()}".encode("utf-8"))
                return self
            except FileExistsError:
                try:
                    age = time.time() - self.path.stat().st_mtime
                except OSError:
                    age = 0.0
                if age > _LOCK_STALE_S:
                    try:
                        self.path.unlink()
                    except OSError:
                        pass
                time.sleep(0.05)

    def __exit__(self, *exc: Any) -> bool:
        if self.fd is not None:
            try:
                os.close(self.fd)
            finally:
                self.fd = None
        try:
            self.path.unlink()
        except OSError:
            pass
        return False


def atomic_append_line(path: str | Path, record: dict[str, Any]) -> Path:
    """把一条记录以 JSONL 形式原子追加到文件末尾，返回文件路径。

    不直接以 'a' 模式写：改成"读旧 + 写临时 + os.replace"，这样即使写到一半
    断电/被杀，账本要么是旧内容、要么是完整新内容，不会出现截断的半行。
    """
    p = Path(path)
    p.parent.mkdir(parents=True, exist_ok=True)
    line = json.dumps(record, ensure_ascii=False)
    lock_path = p.with_name(p.name + ".lock")
    with _FileLock(lock_path):
        old = ""
        if p.is_file():
            old = p.read_text(encoding="utf-8", errors="replace")
            if old and not old.endswith("\n"):
                old += "\n"
        tmp = p.with_name(f"{p.name}.tmp{os.getpid()}")
        try:
            with open(tmp, "w", encoding="utf-8", newline="\n") as fh:
                fh.write(old)
                fh.write(line)
                fh.write("\n")
                fh.flush()
                os.fsync(fh.fileno())
            os.replace(tmp, p)
        finally:
            if tmp.exists():
                try:
                    tmp.unlink()
                except OSError:
                    pass
    return p


# --------------------------------------------------------------------------- #
# 读取
# --------------------------------------------------------------------------- #
def read_all(root: str | Path, spec: Spec | None = None) -> list[dict[str, Any]]:
    """读取全部可解析的记录（损坏行被跳过，由 verify() 负责报告）。"""
    p = ledger_path(root, spec)
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
            continue
        if isinstance(rec, dict):
            out.append(rec)
    return out


def last_record(root: str | Path, spec: Spec | None = None) -> dict[str, Any] | None:
    recs = read_all(root, spec)
    return recs[-1] if recs else None


# --------------------------------------------------------------------------- #
# 写入
# --------------------------------------------------------------------------- #
def ensure(root: str | Path, spec: Spec | None = None) -> Path:
    """确保账本文件存在（空文件也算存在）。

    为什么在跑规则之前就要建：GOV-002 的 audit 检查会在运行过程中读账本，
    "首次运行还没有账本"不应被误判成"删了账本"。
    """
    p = ledger_path(root, spec)
    p.parent.mkdir(parents=True, exist_ok=True)
    if not p.exists():
        p.touch()
    return p


def _base_record(report: GateReport) -> dict[str, Any]:
    # task/commit 用显式占位符而非空串：GOV-002 要求字段非空，"无任务/无 git"
    # 也必须留下可区分的痕迹，否则账本行会被 verify() 判为缺字段并污染后续审计。
    return {
        "event": "run",
        "run_id": report.run_id,
        "task": report.task or "adhoc",
        "commit": report.commit or "unknown",
        "verdict": report.verdict.value,
        "started_at": report.started_at,
        "duration_s": round(report.duration_s, 3),
        "spec_hash": report.spec_hash,
    }


def _warnings(report: GateReport) -> list[dict[str, str]]:
    out: list[dict[str, str]] = []
    for r in report.results:
        for f in r.findings:
            if f.status is Status.WARN:
                out.append({"rule": r.rule_id, "subject": f.subject, "message": f.message})
    return out


def append_run(root: str | Path, report: GateReport, spec: Spec | None = None) -> Path:
    """追加一行运行记录（含 GOV-002 必需字段 + 复核所需的派生字段）。"""
    fields = required_fields(spec)
    rec = _base_record(report)
    rec.update(
        {
            "branch": report.branch,
            "attempt": report.attempt,
            "exit_code": report.exit_code,
            "counts": report.counts(),
            "blocking_rules": [r.rule_id for r in report.results if r.blocking],
            "waived_rules": [r.rule_id for r in report.results if r.status is Status.WAIVED],
            "warnings": _warnings(report),
            "report_dir": report.report_dir,
            "recorded_at": now_iso(),
        }
    )
    missing = [f for f in fields if f not in rec]
    if missing:  # 宁可报错，也不要写出字段残缺却"看起来正常"的账本
        raise SpecError(f"账本记录缺少 GOV-002 必需字段 {missing}；拒绝写入残缺账本")
    return atomic_append_line(ledger_path(root, spec), rec)


def append_event(
    root: str | Path,
    spec: Spec | None,
    event: str,
    task: str = "",
    actor: str = "",
    reason: str = "",
    commit: str = "",
    **extra: Any,
) -> Path:
    """追加一条治理事件（如 reset）。同样带齐 GOV-002 必需字段，保持账本同构。"""
    rec: dict[str, Any] = {
        "event": event,
        "run_id": f"{event}-{time.strftime('%Y%m%dT%H%M%S')}",
        "task": task,
        "commit": commit,
        "verdict": event,
        "started_at": now_iso(),
        "duration_s": 0.0,
        "spec_hash": spec.spec_hash if spec else "",
        "actor": actor,
        "reason": reason,
        "recorded_at": now_iso(),
    }
    rec.update(extra)
    missing = [f for f in required_fields(spec) if f not in rec]
    if missing:
        raise SpecError(f"事件记录缺少 GOV-002 必需字段 {missing}；拒绝写入残缺账本")
    return atomic_append_line(ledger_path(root, spec), rec)


# --------------------------------------------------------------------------- #
# 校验（GOV-002 的 audit 检查会调用）
# --------------------------------------------------------------------------- #
def verify(root: str | Path, spec: Spec | None = None) -> list[str]:
    """逐行检查账本是否含 GOV-002 必需字段。

    返回问题列表；空列表 = 账本结构合法。文件缺失本身也算问题：账本入版本
    控制，凭空消失说明留痕被破坏（首次运行前 runner 会先 ensure 出空文件）。
    """
    problems: list[str] = []
    p = ledger_path(root, spec)
    if not p.is_file():
        problems.append(f"账本文件不存在: {p.name}（GOV-002 要求每次运行必须留痕）")
        return problems

    fields = required_fields(spec)
    for lineno, raw in enumerate(p.read_text(encoding="utf-8", errors="replace").splitlines(), 1):
        if not raw.strip():
            problems.append(f"第 {lineno} 行为空行（JSONL 不允许空行）")
            continue
        try:
            rec = json.loads(raw)
        except json.JSONDecodeError as exc:
            problems.append(f"第 {lineno} 行不是合法 JSON: {exc}")
            continue
        if not isinstance(rec, dict):
            problems.append(f"第 {lineno} 行不是 JSON 对象")
            continue
        missing = [f for f in fields if f not in rec or rec[f] is None or rec[f] == ""]
        if missing:
            problems.append(f"第 {lineno} 行缺少必需字段: {missing}")
    return problems
