"""门禁数据模型：Finding / RuleResult / GateReport / 状态与退出码。

设计原则（为什么这样切分）：
  - Finding 是"最小可判定事实"，必须自带 observed/threshold，人类无需回看日志即可判断。
  - RuleResult 是"一条规则的归宿"，把机器判定与人工判断（waiver）分开存放，避免糊在一起。
  - GateReport 是"一次运行的完整快照"，可序列化为 JSON（机器）与 Markdown（人类）。
  - 退出码固定，供 CI 与本地 hooks 统一消费。
"""
from __future__ import annotations

import dataclasses
import json
import time
import uuid
from dataclasses import dataclass, field
from enum import Enum
from typing import Any


# --------------------------------------------------------------------------- #
# 枚举
# --------------------------------------------------------------------------- #
class Status(str, Enum):
    """单条 finding / 规则的归宿。"""

    PASS = "pass"        # 通过
    BLOCK = "block"      # 阻断
    WARN = "warn"        # 警告（记账不阻断）
    WAIVED = "waived"    # 有有效例外（人已批准）
    MANUAL = "manual"    # 转人工判断（未批准前按阻断处理）
    HALTED = "halted"    # 重试超限，禁止继续自动尝试
    ERROR = "error"      # 检查自身出错（环境/脚本问题，不是代码问题）

    @property
    def is_failing(self) -> bool:
        return self in (Status.BLOCK, Status.MANUAL, Status.HALTED, Status.ERROR)


class ExitCode:
    PASS = 0
    BLOCKED = 1
    HALTED = 2
    SPEC_ERROR = 3
    ENV_ERROR = 4


STATUS_ICON = {
    Status.PASS: "✓",
    Status.BLOCK: "✗",
    Status.WARN: "⚠",
    Status.WAIVED: "⊘",
    Status.MANUAL: "⏸",
    Status.HALTED: "⛔",
    Status.ERROR: "💥",
}


# --------------------------------------------------------------------------- #
# 事实与判定
# --------------------------------------------------------------------------- #
@dataclass
class Finding:
    """一条可判定事实。

    observed/threshold 必填（除非 status 为 PASS/ERROR），保证人类能独立复核。
    """

    rule: str
    status: Status
    subject: str
    message: str
    observed: Any = None
    threshold: Any = None
    level: str = "error"           # error | warning | info
    evidence: str | None = None    # 证据文件相对路径
    hint: str | None = None        # 修复指引（可直接照做）
    line: int | None = None

    def to_dict(self) -> dict[str, Any]:
        d = dataclasses.asdict(self)
        d["status"] = self.status.value
        return d


@dataclass
class RuleResult:
    """一条规则的执行归宿。"""

    rule_id: str
    title: str
    mode: str                      # block | warn | manual | ratchet
    findings: list[Finding] = field(default_factory=list)
    duration_ms: int = 0
    waived_by: str | None = None
    waiver_id: str | None = None
    error: str | None = None

    @property
    def status(self) -> Status:
        if self.error:
            return Status.ERROR
        if not self.findings:
            return Status.PASS
        if all(f.status is Status.PASS for f in self.findings):
            return Status.PASS
        # 有 waived 的规则整体记为 waived（但保留逐条明细）
        real = [f for f in self.findings if f.status is not Status.PASS]
        if real and all(f.status is Status.WAIVED for f in real):
            return Status.WAIVED
        if any(f.status is Status.HALTED for f in real):
            return Status.HALTED
        if any(f.status is Status.ERROR for f in real):
            return Status.ERROR
        if any(f.status is Status.MANUAL for f in real):
            return Status.MANUAL
        if any(f.status is Status.BLOCK for f in real):
            return Status.BLOCK
        if any(f.status is Status.WARN for f in real):
            return Status.WARN
        return Status.PASS

    @property
    def blocking(self) -> bool:
        return self.status.is_failing

    def to_dict(self) -> dict[str, Any]:
        return {
            "rule_id": self.rule_id,
            "title": self.title,
            "mode": self.mode,
            "status": self.status.value,
            "duration_ms": self.duration_ms,
            "waived_by": self.waived_by,
            "waiver_id": self.waiver_id,
            "error": self.error,
            "findings": [f.to_dict() for f in self.findings],
        }


# --------------------------------------------------------------------------- #
# 运行快照
# --------------------------------------------------------------------------- #
def new_run_id(task: str, attempt: int) -> str:
    stamp = time.strftime("%Y%m%dT%H%M%S", time.localtime())
    short = uuid.uuid4().hex[:4]
    safe_task = "".join(c if c.isalnum() or c in "-_" else "-" for c in (task or "adhoc"))
    return f"{stamp}-{safe_task}-a{attempt}-{short}"


@dataclass
class GateReport:
    """一次门禁运行的完整结果（机器可读 + 人类可读）。"""

    run_id: str
    task: str
    attempt: int
    commit: str
    branch: str
    started_at: str
    duration_s: float
    verdict: Status
    exit_code: int
    results: list[RuleResult] = field(default_factory=list)
    environment: dict[str, Any] = field(default_factory=dict)
    spec_hash: str = ""
    report_dir: str = ""

    # ---- 派生视图 ---------------------------------------------------------- #
    @property
    def blocking_results(self) -> list[RuleResult]:
        return [r for r in self.results if r.blocking]

    @property
    def waived_results(self) -> list[RuleResult]:
        return [r for r in self.results if r.status is Status.WAIVED]

    def counts(self) -> dict[str, int]:
        out: dict[str, int] = {}
        for r in self.results:
            out[r.status.value] = out.get(r.status.value, 0) + 1
        return out

    def to_dict(self) -> dict[str, Any]:
        return {
            "run_id": self.run_id,
            "task": self.task,
            "attempt": self.attempt,
            "commit": self.commit,
            "branch": self.branch,
            "started_at": self.started_at,
            "duration_s": round(self.duration_s, 3),
            "verdict": self.verdict.value,
            "exit_code": self.exit_code,
            "spec_hash": self.spec_hash,
            "environment": self.environment,
            "counts": self.counts(),
            "results": [r.to_dict() for r in self.results],
        }

    def to_json(self) -> str:
        return json.dumps(self.to_dict(), ensure_ascii=False, indent=2)
