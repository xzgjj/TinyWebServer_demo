"""门禁规格加载与校验（gate/spec.toml 是规则唯一真相源）。

约定：
  * 代码中不得硬编码阈值，一律从 Spec 读取。
  * 校验失败抛 SpecError -> CLI 以退出码 3 结束（避免"规则坏了却静默通过"）。
"""
from __future__ import annotations

import hashlib
import re
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

try:  # Python >= 3.11
    import tomllib
except ModuleNotFoundError as exc:  # pragma: no cover - 环境问题，明确报错而非静默降级
    raise SystemExit("需要 Python >= 3.11（tomllib）。当前解释器不满足。") from exc


VALID_MODES = {"block", "warn", "manual", "ratchet"}

# 与 scripts/gate/checks/__init__.py 的 REGISTRY 必须一致，由 runner 在启动时双向校验。
KNOWN_CHECKS = {
    "doc_budget",
    "doc_dup",
    "doc_refs",
    "dead_code",
    "dup_code",
    "scope",
    "diff_budget",
    "lint",
    "complexity",
    "problems_panel",
    "std_features",
    "protected_tests",
    "test_coverage",
    "rule_guard",
    "retry_cap",
    "audit",
}

RULE_ID_RE = re.compile(r"^[A-Z][A-Z0-9]*-\d{3}$")


class SpecError(Exception):
    """规格文件本身不合法。"""


@dataclass(frozen=True)
class Rule:
    id: str
    title: str
    mode: str
    check: str
    desc: str = ""
    params: dict[str, Any] = field(default_factory=dict)

    def get(self, key: str, default: Any = None) -> Any:
        return self.params.get(key, default)


@dataclass
class Spec:
    path: Path
    meta: dict[str, Any]
    rules: dict[str, Rule]
    raw: dict[str, Any]
    spec_hash: str

    # ---- 常用视图 ---------------------------------------------------------- #
    @property
    def max_gate_attempts(self) -> int:
        return int(self.meta.get("max_gate_attempts", 3))

    @property
    def rule_approvers(self) -> list[str]:
        return list(self.meta.get("rule_approvers", []))

    @property
    def protected_paths(self) -> list[str]:
        return list(self.meta.get("protected_paths", []))

    @property
    def exclude_globs(self) -> list[str]:
        return list(self.meta.get("exclude_globs", []))

    @property
    def code_exts(self) -> list[str]:
        return list(self.meta.get("code_exts", [".cpp", ".h", ".hpp"]))

    def rule(self, rule_id: str) -> Rule:
        try:
            return self.rules[rule_id]
        except KeyError as exc:
            raise SpecError(f"未定义的规则: {rule_id}") from exc

    def path_of(self, key: str, default: str) -> Path:
        return Path(self.meta.get(key, default))

    # ---- 加载 -------------------------------------------------------------- #
    @classmethod
    def load(cls, path: str | Path) -> "Spec":
        p = Path(path)
        if not p.is_file():
            raise SpecError(f"规格文件不存在: {p}")
        raw_bytes = p.read_bytes()
        try:
            data = tomllib.loads(raw_bytes.decode("utf-8"))
        except Exception as exc:  # tomllib.TOMLDecodeError 等
            raise SpecError(f"TOML 解析失败: {p}: {exc}") from exc

        meta = data.get("meta")
        if not isinstance(meta, dict):
            raise SpecError("缺少 [meta] 段")
        if not meta.get("rule_approvers"):
            raise SpecError("[meta].rule_approvers 不能为空（CI-001 依赖它）")
        max_attempts = int(meta.get("max_gate_attempts", 0))
        if max_attempts <= 0:
            raise SpecError("[meta].max_gate_attempts 必须为正整数（GOV-001 依赖它）")

        rules_raw = data.get("rules")
        if not isinstance(rules_raw, dict) or not rules_raw:
            raise SpecError("缺少 [rules.<ID>] 段")

        rules: dict[str, Rule] = {}
        for rid, body in rules_raw.items():
            if not RULE_ID_RE.match(rid):
                raise SpecError(f"规则 ID 命名不合法: {rid}（应形如 DOC-001）")
            if not isinstance(body, dict):
                raise SpecError(f"规则 {rid} 定义不是表结构")
            for required in ("title", "mode", "check"):
                if not body.get(required):
                    raise SpecError(f"规则 {rid} 缺少字段 {required}")
            mode = str(body["mode"])
            if mode not in VALID_MODES:
                raise SpecError(f"规则 {rid} 的 mode={mode} 不合法，可选: {sorted(VALID_MODES)}")
            check = str(body["check"])
            if check not in KNOWN_CHECKS:
                raise SpecError(
                    f"规则 {rid} 引用了未知 check={check}；"
                    f"可选: {', '.join(sorted(KNOWN_CHECKS))}"
                )
            params = body.get("params", {})
            if not isinstance(params, dict):
                raise SpecError(f"规则 {rid} 的 params 必须是表结构")
            for k, v in params.items():
                if isinstance(v, (int, float)) and not isinstance(v, bool) and v < 0:
                    raise SpecError(f"规则 {rid} 的阈值 {k}={v} 不能为负")
            rules[rid] = Rule(
                id=rid,
                title=str(body["title"]),
                mode=mode,
                check=check,
                desc=str(body.get("desc", "")),
                params=params,
            )

        if len(rules) != len(set(rules)):
            raise SpecError("规则 ID 重复")

        return cls(
            path=p,
            meta=dict(meta),
            rules=rules,
            raw=data,
            spec_hash=hashlib.sha256(raw_bytes).hexdigest(),
        )

    def check_registry_consistency(self, registry: dict[str, Any]) -> None:
        """spec 声明的 check 与代码注册表必须双向一致（防止"规则形同虚设"）。"""
        spec_checks = {r.check for r in self.rules.values()}
        missing_impl = spec_checks - set(registry)
        unused_impl = set(registry) - spec_checks
        if missing_impl:
            raise SpecError(f"以下 check 在 spec 中声明但未实现: {sorted(missing_impl)}")
        if unused_impl:
            raise SpecError(f"以下 check 已实现但未被任何规则引用: {sorted(unused_impl)}")
