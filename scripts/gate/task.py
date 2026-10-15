"""任务契约：Agent 与人类之间的"接口"。

一个任务 = 一份契约 = { 验收标准, 允许改动范围, diff 预算, 受保护资产, 尝试次数 }。
门禁永远按"当前契约"判定；没有契约时 SCOPE-001 直接阻断（防止无人认领的改动）。
"""
from __future__ import annotations

import json
import time
from dataclasses import asdict, dataclass, field
from pathlib import Path
from typing import Any


class TaskError(Exception):
    pass


@dataclass
class TaskContract:
    id: str
    title: str = ""
    phase: str = ""
    owner: str = "agent"
    state: str = "DECLARED"          # DECLARED | RUNNING | FAILED | PASSED | HALTED | COMMITTED
    attempts: int = 0
    acceptance: list[str] = field(default_factory=list)
    allow: list[str] = field(default_factory=list)
    deny: list[str] = field(default_factory=list)
    max_changed_lines: int | None = None
    min_changed_lines: int | None = None
    max_changed_files: int | None = None
    protected: list[str] = field(default_factory=list)
    audit_tag: str = ""              # 例: audit-04
    base_ref: str = ""               # 空=以工作区相对 HEAD 判定；CI 用 A..B
    created_at: str = ""
    updated_at: str = ""
    revision: int = 1
    notes: list[str] = field(default_factory=list)

    # ---- 序列化 ------------------------------------------------------------ #
    def to_dict(self) -> dict[str, Any]:
        return asdict(self)

    @classmethod
    def from_dict(cls, data: dict[str, Any]) -> "TaskContract":
        known = {f for f in cls.__dataclass_fields__}  # type: ignore[attr-defined]
        unknown = set(data) - known
        if unknown:
            raise TaskError(f"任务契约包含未知字段: {sorted(unknown)}")
        if not data.get("id"):
            raise TaskError("任务契约缺少 id")
        obj = cls(**{k: v for k, v in data.items() if k in known})
        if obj.max_changed_lines is not None and obj.max_changed_lines <= 0:
            raise TaskError("max_changed_lines 必须为正")
        if not obj.created_at:
            obj.created_at = now_iso()
        return obj

    @classmethod
    def load(cls, path: str | Path) -> "TaskContract":
        p = Path(path)
        if not p.is_file():
            raise TaskError(f"任务契约不存在: {p}")
        try:
            data = json.loads(p.read_text(encoding="utf-8"))
        except json.JSONDecodeError as exc:
            raise TaskError(f"任务契约 JSON 解析失败: {p}: {exc}") from exc
        return cls.from_dict(data)

    def save(self, path: str | Path) -> Path:
        p = Path(path)
        p.parent.mkdir(parents=True, exist_ok=True)
        self.updated_at = now_iso()
        self.revision += 1
        p.write_text(json.dumps(self.to_dict(), ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
        return p

    # ---- 行为 -------------------------------------------------------------- #
    def register_attempt(self, verdict: str) -> None:
        """每次 gate run 记一次尝试；PASS 后归零，避免历史失败永久累计。"""
        self.state = "PASSED" if verdict == "pass" else "FAILED"
        if verdict == "pass":
            self.attempts = 0
        else:
            self.attempts += 1

    def effective_limits(self, spec_rule: Any) -> tuple[int, int, int]:
        """返回 (max_lines, max_files, min_lines)，契约优先，其次 spec 默认。"""
        max_lines = self.max_changed_lines or int(spec_rule.get("max_changed_lines", 800))
        max_files = self.max_changed_files or int(spec_rule.get("max_changed_files", 24))
        min_lines = self.min_changed_lines if self.min_changed_lines is not None else int(
            spec_rule.get("min_changed_lines", 0)
        )
        return max_lines, max_files, min_lines


def now_iso() -> str:
    return time.strftime("%Y-%m-%dT%H:%M:%S%z")
