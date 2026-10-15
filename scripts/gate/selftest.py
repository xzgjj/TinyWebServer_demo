"""门禁规则负向回归自测：证明"该红时一定红"。

用法：
    python -X utf8 scripts/gate/selftest.py                      # 跑全部 16 条规则
    python -X utf8 scripts/gate/selftest.py --list               # 只列场景
    python -X utf8 scripts/gate/selftest.py --only DOC-001 -v    # 只跑一条并打印明细
    python -X utf8 scripts/gate/selftest.py --update-fixture-baselines [--baselines-only]
                                                                 # 重算夹具基线

设计：
  * 每条规则一个负向夹具（gate/fixtures/<RULE_ID>/case.json + files/），断言该规则
    至少产出一条 block/manual/halted 类 finding；
  * 另有一个"干净"正向夹具，断言不误报（不允许出现 block/manual/halted/error）；
  * 夹具在临时目录里展开成假仓库，直接实例化 Context，不触碰真实仓库文件；
  * 唯一例外：`--update-fixture-baselines` 会写夹具目录下的基线 JSON（显式请求才写）。

夹具是"可独立复核的输入"：case.json 记录期望归宿与假 git 事实，files/ 放最小输入文件，
两者都可手工编辑。改了受保护测试文件或文档字数后，跑一次
`--update-fixture-baselines --baselines-only` 重算基线即可。

不做的事：不修改真实仓库、不生成 build/、不网络访问。
"""
from __future__ import annotations

import argparse
import importlib
import json
import os
import shutil
import subprocess
import sys
import tempfile
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Iterator

# --- 让 `gate.*` 可导入（scripts/ 下有 gate 包，仓库根用于相对路径） ---------- #
_HERE = Path(__file__).resolve()
REPO_ROOT = _HERE.parents[2]
SCRIPTS_DIR = REPO_ROOT / "scripts"
for candidate in (str(SCRIPTS_DIR), str(REPO_ROOT)):
    if candidate not in sys.path:
        sys.path.insert(0, candidate)

from gate.checks import REGISTRY                                  # noqa: E402
from gate.context import Context, GitFacts                        # noqa: E402
from gate.model import Finding, Status, new_run_id                # noqa: E402
from gate.spec import Rule, Spec                                  # noqa: E402
from gate.task import TaskContract                                # noqa: E402

FIXTURES_DIR = REPO_ROOT / "gate" / "fixtures"
CLEAN_FIXTURE = "_clean"
FAILING = {Status.BLOCK, Status.MANUAL, Status.HALTED}


# --------------------------------------------------------------------------- #
# 默认规格（与 gate/spec.toml 保持同步：check 归属/模式/阈值）
# --------------------------------------------------------------------------- #
# 注意：这份内嵌规格只服务自测。每次运行都会做"与真实 spec.toml 的一致性核对"，
# 任何漂移都会被报出来，避免自测在错误的规格下自证清白。
DEFAULT_RULES: dict[str, dict[str, Any]] = {
    "DOC-001": {
        "title": "文档字数预算", "mode": "block", "check": "doc_budget",
        "params": {
            "unit": "non_whitespace_chars",
            "budgets": {"SPEC.md": 3000, "CLI.md": 3000,
                        "AGENT_EXECUTION_PROTOCOL.md": 3000},
            "grandfathered": ["CLAUDE.md", "implementation_plan.md"],
            "unlimited_globs": ["doc/**", "notes.txt", "diff.md", "audit_report.md", "README.md"],
        },
    },
    "DOC-002": {
        "title": "重复段落检查", "mode": "block", "check": "doc_dup",
        "params": {"min_block_chars": 40, "max_intra_dup": 0, "max_cross_ratio": 0.05},
    },
    "DOC-003": {
        "title": "文档-代码一致性", "mode": "block", "check": "doc_refs",
        "params": {
            "docs": ["CLAUDE.md", "README.md", "SPEC.md", "CLI.md",
                     "AGENT_EXECUTION_PROTOCOL.md", "doc/architecture_overview.md"],
            "check_paths": True, "check_cxx_std": True,
        },
    },
    "CODE-001": {
        "title": "死代码与未使用符号", "mode": "block", "check": "dead_code",
        "params": {"enabled": True, "max_commented_block_lines": 12,
                   "max_unused_private_fields": 0, "max_unused_static_functions": 0},
    },
    "CODE-002": {
        "title": "重复代码块", "mode": "warn", "check": "dup_code",
        "params": {"min_lines": 8, "max_dup_blocks": 2},
    },
    "SCOPE-001": {
        "title": "改动范围白名单", "mode": "block", "check": "scope",
        "params": {"default_deny": ["third_party/**", "public/**", "LICENSE"],
                   "require_task_contract": True},
    },
    "SCOPE-002": {
        "title": "Diff 预算", "mode": "block", "check": "diff_budget",
        "params": {"max_changed_lines": 800, "min_changed_lines": 0,
                   "max_changed_files": 24,
                   "max_changed_lines_cap": 25000, "max_changed_files_cap": 600,
                   "count_mode": "added_plus_deleted"},
    },
    "QUAL-001": {
        "title": "编译警告纪律", "mode": "block", "check": "lint",
        "params": {"compilers": ["g++", "clang++"], "standard": "c++23",
                   "flags": ["-Wall", "-Wextra", "-Wpedantic", "-Wconversion", "-Wsign-conversion"],
                   "max_new_warnings": 0, "timeout_s": 900},
    },
    "QUAL-002": {
        "title": "复杂度上限", "mode": "block", "check": "complexity",
        "params": {"max_cyclomatic": 15, "max_function_lines": 80, "max_nesting": 4,
                   "scope": "changed_files"},
    },
    "QUAL-003": {
        "title": "编辑器问题面板为零", "mode": "block", "check": "problems_panel",
        "params": {"require_compile_commands": True, "cppcheck_enable": True,
                   "cppcheck_max_errors": 0},
    },
    "STD-001": {
        "title": "C++ 标准特性白名单", "mode": "block", "check": "std_features",
        "params": {
            "allowed": ["std::to_underlying", "std::string::contains", "std::byteswap",
                        "std::unreachable", "if consteval", "std::span", "std::optional",
                        "std::string_view", "std::filesystem", "std::jthread",
                        "std::source_location", "std::format", "std::bit_cast", "concepts"],
            "forbidden_unless_guarded": ["<print>", "<mdspan>", "<flat_map>", "std::expected",
                                         "std::stacktrace", "deducing this", "std::mdspan"],
            "guard_macro": "TWS_HAS_",
        },
    },
    "TEST-001": {
        "title": "受保护回归测试", "mode": "manual", "check": "protected_tests",
        "params": {"protected_globs": ["test/**", "benchmark/src/**"],
                   "baseline_file": "gate/protected_tests.json",
                   "block_on_hash_change": False, "block_on_case_decrease": True,
                   "block_on_delete": True},
    },
    "TEST-002": {
        "title": "新增能力必须带测试", "mode": "block", "check": "test_coverage",
        "params": {"src_globs": ["src/**/*.cpp", "reactor/**/*.cpp"], "test_dir": "test",
                   "cmake_file": "CMakeLists.txt", "require_ctest_smoke": True,
                   "smoke_label": "smoke"},
    },
    "CI-001": {
        "title": "规则文件变更须指定人批准", "mode": "block", "check": "rule_guard",
        "params": {"require_waiver": True, "require_distinct_actor": True,
                   "waiver_max_age_days": 30},
    },
    "GOV-001": {
        "title": "门禁重试轮数上限", "mode": "block", "check": "retry_cap",
        "params": {"max_attempts": 3, "reset_requires_human": True},
    },
    "GOV-002": {
        "title": "审计留痕", "mode": "block", "check": "audit",
        "params": {"required_fields": ["run_id", "task", "commit", "verdict", "started_at",
                                       "duration_s", "spec_hash"]},
    },
}

DEFAULT_META: dict[str, Any] = {
    "spec_version": "1.0.0",
    "project": "TinyWebServer_demo",
    "max_gate_attempts": 3,
    "rule_approvers": ["xzgjj"],
    "report_dir": "local/gate",
    "ledger_path": "gate/ledger.jsonl",
    "waiver_path": "gate/waivers.jsonl",
    "task_dir": "gate/tasks",
    "baseline_path": "gate/baseline.json",
    "protected_paths": ["gate/spec.toml", "gate/baseline.json", "gate/protected_tests.json",
                        "gate/tasks/**", "scripts/gate/**", ".github/workflows/**",
                        "CODEOWNERS", "CMakeLists.txt"],
    "exclude_globs": ["build/**", "build_*/**", "third_party/**", "benchmark_results/**",
                      "local/**", "__pycache__/**", "**/__pycache__/**", ".git/**",
                      "Testing/**", "audit_report_backups/**", "test_result/**", "*.pyc",
                      # 负向夹具里故意写坏代码，属测试数据而非产品代码（与 spec.toml 保持一致）
                      "gate/fixtures/**"],
    "code_exts": [".cpp", ".h", ".hpp", ".cc", ".cxx", ".ipp", ".inl"],
}


# --------------------------------------------------------------------------- #
# 夹具场景
# --------------------------------------------------------------------------- #
@dataclass
class Scenario:
    rule_id: str
    title: str
    kind: str                     # negative | clean
    path: Path
    case: dict[str, Any]
    label: str = ""

    @property
    def id(self) -> str:
        return self.rule_id if not self.label else f"{self.rule_id}:{self.label}"

    @property
    def expect(self) -> set[Status]:
        raw = self.case.get("expect_status") or ["block"]
        return {Status(x) for x in raw}

    @property
    def expect_none(self) -> set[Status]:
        raw = self.case.get("forbid_status") or ["block", "manual", "halted", "error"]
        return {Status(x) for x in raw}


def _load_cases(directory: Path) -> list[tuple[str, dict[str, Any]]]:
    """一个夹具目录里可以有多个用例：case.json、case_alt.json…（同一规则的多条断言）。"""
    out: list[tuple[str, dict[str, Any]]] = []
    for case_path in sorted(directory.glob("case*.json")):
        label = "" if case_path.name == "case.json" else case_path.stem
        try:
            out.append((label, json.loads(case_path.read_text(encoding="utf-8"))))
        except json.JSONDecodeError as exc:
            out.append((label, {"_error": f"{case_path.name} 解析失败: {exc}"}))
    return out


def discover_scenarios() -> tuple[list[Scenario], list[str]]:
    """返回 (场景列表, 问题列表)。缺夹具/多余夹具都会被报成问题。"""
    problems: list[str] = []
    scenarios: list[Scenario] = []
    if not FIXTURES_DIR.is_dir():
        return [], [f"夹具目录不存在: {FIXTURES_DIR}"]

    expected = set(DEFAULT_RULES)          # 规则 ID（DOC-001…GOV-002），不是 check 名
    for child in sorted(FIXTURES_DIR.iterdir()):
        if child.name.startswith("_") or not child.is_dir():
            continue
        cases = _load_cases(child)
        if not cases:
            problems.append(f"{child.name}/ 缺少 case.json")
            continue
        for label, case in cases:
            if "_error" in case:
                problems.append(f"{child.name}/{label}: {case['_error']}")
                continue
            rule_id = str(case.get("rule", child.name))
            if rule_id != child.name:
                problems.append(f"{child.name}/ 的 case rule={rule_id} 与目录名不一致")
            if rule_id not in expected:
                problems.append(f"{child.name}/ 不是已注册规则（{rule_id}）")
                continue
            scenario = Scenario(
                rule_id=rule_id, title=str(case.get("title", "")), kind="negative",
                path=child, case=case, label=label,
            )
            # 用例可用 `_files_dir` 指向自己的文件目录（相对夹具目录或绝对路径）
            raw_files_dir = str(case.get("_files_dir") or "files")
            files_dir = Path(raw_files_dir)
            scenario.case["_files_dir"] = str(
                files_dir if files_dir.is_absolute() else child / files_dir
            )
            scenarios.append(scenario)

    got = {s.rule_id for s in scenarios}
    for missing in sorted(expected - got):
        problems.append(f"缺少规则 {missing} 的负向夹具（gate/fixtures/{missing}/）")

    clean_dir = FIXTURES_DIR / CLEAN_FIXTURE
    if not (clean_dir / "case.json").is_file():
        problems.append(f"缺少正向夹具 {CLEAN_FIXTURE}/case.json")
    else:
        _, case = _load_cases(clean_dir)[0]
        case["_files_dir"] = str(clean_dir / "files")
        case["_allow_missing_baseline"] = True
        scenarios.append(
            Scenario(rule_id=CLEAN_FIXTURE, title=str(case.get("title", "")), kind="clean",
                     path=clean_dir, case=case)
        )
    return scenarios, problems


def REGISTRY_TO_RULES() -> dict[str, str]:
    """{check 名: rule_id}：由内嵌默认规格推出，反向校验夹具覆盖了全部规则。"""
    return {str(body["check"]): rid for rid, body in DEFAULT_RULES.items()}


# --------------------------------------------------------------------------- #
# 规格构建 / 夹具展开
# --------------------------------------------------------------------------- #
def render_spec_toml(overrides: dict[str, Any] | None = None) -> str:
    """把内嵌规格渲染成 TOML（夹具可通过 case.json 的 override.params 覆写阈值）。"""
    rules = json.loads(json.dumps(DEFAULT_RULES))       # 深拷贝
    for rid, patch in (overrides or {}).items():
        if rid not in rules:
            raise KeyError(f"夹具覆写了不存在的规则: {rid}")
        for key, value in (patch or {}).items():
            rules[rid]["params"][key] = value

    out: list[str] = [
        "# 自测专用规格（由 selftest.py 渲染；与 scripts/gate/checks 的判定器一一对应）",
        "",
        "[meta]",
    ]
    for key, value in DEFAULT_META.items():
        out.append(f"{key} = {_toml(value)}")
    for rid, body in rules.items():
        out.append("")
        out.append(f"[rules.{rid}]")
        out.append(f"title = {_toml(body['title'])}")
        out.append(f"mode  = {_toml(body['mode'])}")
        out.append(f"check = {_toml(body['check'])}")
        out.append(f"desc  = {_toml(f'自测夹具 {rid}')}")
        out.append(f"[rules.{rid}.params]")
        for key, value in body["params"].items():
            out.append(f"{key} = {_toml(value)}")
    return "\n".join(out) + "\n"


def _toml(value: Any) -> str:
    if isinstance(value, bool):
        return "true" if value else "false"
    if isinstance(value, (int, float)):
        return str(value)
    if isinstance(value, str):
        return json.dumps(value, ensure_ascii=False)
    if isinstance(value, (list, tuple)):
        return "[" + ", ".join(_toml(v) for v in value) + "]"
    if isinstance(value, dict):
        return "{ " + ", ".join(f"{json.dumps(str(k), ensure_ascii=False)} = {_toml(v)}"
                                for k, v in value.items()) + " }"
    raise TypeError(f"无法渲染为 TOML 的值: {value!r}")


def copy_fixture_tree(case: dict[str, Any], work: Path) -> None:
    """把 case.json 描述的假仓库写进临时目录。"""
    for rel, text in (case.get("files") or {}).items():
        _write(work / rel, str(text))
    files_dir = case.get("_files_dir")
    if files_dir and Path(files_dir).is_dir():
        for src in sorted(Path(files_dir).rglob("*")):
            if src.is_file():
                dst = work / src.relative_to(files_dir)
                dst.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(src, dst)
    for rel, text in (case.get("text_files") or {}).items():
        _write(work / rel, str(text))
    for rel, name in (case.get("baselines") or {}).items():
        src = FIXTURES_DIR / CLEAN_FIXTURE / "baseline" / str(name)
        if not src.is_file():
            # 生成基线时允许缺失（--update-fixture-baselines 自己就会写出这些文件）
            if case.get("_allow_missing_baseline"):
                continue
            raise FileNotFoundError(f"夹具基线缺失: {src}（先跑 --update-fixture-baselines）")
        shutil.copy2(src, work / rel)


def _write(path: Path, text: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8", newline="\n")


def make_context(work: Path, case: dict[str, Any], rule_id: str) -> tuple[Context, Rule]:
    override = (case.get("override") or {}).get("params") or {}
    spec_text = render_spec_toml(override)
    _write(work / "gate" / "spec.toml", spec_text)
    spec = Spec.load(work / "gate" / "spec.toml")

    task: TaskContract | None = None
    task_case = case.get("task")
    if task_case:
        data = {
            "id": "SELFTEST-001", "title": "自测夹具任务", "state": "RUNNING",
            "owner": "agent", "allow": ["**/*"], "deny": [], "attempts": 0,
        }
        data.update(task_case)
        task = TaskContract.from_dict(data)

    git = GitFacts(**{**{"commit": "deadbee", "branch": "selftest", "author_name": "selftest-bot",
                         "author_email": "selftest@example.com"}, **(case.get("git") or {})})
    options = dict(case.get("options") or {})
    ctx = Context(root=work, spec=spec, task=task, run_id=new_run_id(f"selftest-{rule_id}", 0),
                  git=git, options=options)
    # 夹具声明的"新鲜文件"（用于 ctest 记录新鲜度判定）
    for rel in case.get("mtime_now") or []:
        fp = work / rel
        if fp.exists():
            os.utime(fp, None)
    _ = time
    return ctx, spec.rule(rule_id)


# --------------------------------------------------------------------------- #
# fake 模块（另一代理尚未交付 waivers/ledger；夹具用 fake 模块隔离依赖）
# --------------------------------------------------------------------------- #
def _module(name: str, **attrs: Any) -> Any:
    mod = type(sys)(name)
    for key, value in attrs.items():
        setattr(mod, key, value)
    return mod


def _make_fake(name: str) -> Any:
    if name == "waivers_empty":
        return _module("gate.waivers", active_waivers=lambda *a, **k: [])
    if name == "ledger_ok":
        return _module("gate.ledger", verify=lambda *a, **k: [])
    if name == "ledger_bad":
        return _module("gate.ledger", verify=lambda *a, **k: ["run_id 缺失（夹具）"])
    raise ValueError(f"未知 fake 模块: {name}")


def _install_fakes(names: list[str]) -> dict[str, Any]:
    """安装 fake 模块，返回"原样快照"（用于还原）。

    两个坑都踩过：
      1. 必须用赋值而不是 setdefault：上一个场景装的 fake 会"黏"到下一个场景；
      2. 退出时必须还原成**原来的模块对象**（而不是删掉了事）——真实仓库里
         `scripts/gate/waivers.py`、`ledger.py` 已存在，删掉表项只会让下次 import
         重新加载，而"当前 import"早已拿到 fake，判定结果就失真了。
    """
    snapshot: dict[str, Any] = {}
    for name in names:
        mod = _make_fake(name)
        snapshot[mod.__name__] = sys.modules.get(mod.__name__)
        sys.modules[mod.__name__] = mod
    return snapshot


def _restore_fakes(snapshot: dict[str, Any]) -> None:
    for name, original in snapshot.items():
        if original is None:
            sys.modules.pop(name, None)
        else:
            sys.modules[name] = original


# --------------------------------------------------------------------------- #
# 断言与输出
# --------------------------------------------------------------------------- #
def run_check(ctx: Context, rule: Rule) -> tuple[list[Finding], str | None]:
    fn = REGISTRY.get(rule.check)
    if fn is None:
        return [], f"check {rule.check} 未注册"
    try:
        findings = fn(ctx, rule)
    except Exception as exc:  # noqa: BLE001 - 自测要把异常当成失败，而不是崩掉
        return [], f"{type(exc).__name__}: {exc}"
    if not isinstance(findings, list):
        return [], f"check 返回值不是 list（{type(findings).__name__}）"
    return findings, None


def evaluate(scenario: Scenario, findings: list[Finding]) -> tuple[bool, str]:
    if not findings:
        return False, "没有产出任何 finding（规则形同虚设）"
    if scenario.kind == "negative":
        hit = [f for f in findings if f.status in scenario.expect]
        if not hit:
            got = sorted({f.status.value for f in findings})
            return False, f"未产生 {sorted(s.value for s in scenario.expect)} 类 finding，实际 {got}"
        f = hit[0]
        extra = ""
        if f.observed is None or f.threshold is None:
            extra = "（注意：observed/threshold 缺失，人类无法独立复核）"
        return True, f"{f.status.value}: {f.subject} — {f.message[:120]}{extra}"
    bad = [f for f in findings if f.status in scenario.expect_none]
    if bad:
        return False, f"干净场景误报 {bad[0].status.value}: {bad[0].subject} — {bad[0].message[:140]}"
    return True, f"无误报（{len(findings)} 条 finding，最高级别 pass/warn）"


def check_floor(scenario: Scenario, case: dict[str, Any]) -> str | None:
    """夹具声明的"最低证据量"（例如至少 1 条 block 明细）。"""
    floor = case.get("min_findings")
    if floor is None:
        return None
    return f"夹具声明 min_findings={floor}（提示：{case.get('note', '')}）"


# --------------------------------------------------------------------------- #
# 规格一致性核对（内嵌默认规格 vs 真实 gate/spec.toml）
# --------------------------------------------------------------------------- #
def verify_spec_drift() -> list[str]:
    problems: list[str] = []
    real_path = REPO_ROOT / "gate" / "spec.toml"
    try:
        real = Spec.load(real_path)
    except Exception as exc:  # noqa: BLE001
        return [f"无法加载真实规格 {real_path}: {exc}"]
    if set(real.rules) != set(DEFAULT_RULES):
        problems.append(
            f"规则集合与 gate/spec.toml 不一致：真实={sorted(real.rules)} "
            f"内嵌={sorted(DEFAULT_RULES)}"
        )
    for rid, body in DEFAULT_RULES.items():
        r = real.rules.get(rid)
        if r is None:
            continue
        if r.check != body["check"] or r.mode != body["mode"]:
            problems.append(f"{rid}: 内嵌 check/mode={body['check']}/{body['mode']} != 真实 {r.check}/{r.mode}")
        if set(r.params) != set(body["params"]):
            problems.append(f"{rid}: 内嵌 params 键与真实不一致（真实 {sorted(r.params)}）")
        for key, value in body["params"].items():
            real_value = r.params.get(key)
            if isinstance(value, (list, dict)) and not isinstance(real_value, (list, dict)):
                problems.append(f"{rid}.{key}: 真实 spec 里不是集合类型")
                continue
            # 值也要比：只比键集合会漏掉"阈值被改但键没变"的漂移 ——
            # 那样 selftest 验的是内嵌规格，而不是线上真正生效的规则（曾实测漏判）。
            if isinstance(value, dict) and isinstance(real_value, dict):
                if {str(k): v for k, v in value.items()} != {str(k): v for k, v in real_value.items()}:
                    problems.append(f"{rid}.{key}: 内嵌 {value} != 真实 {real_value}")
            elif isinstance(value, list) and isinstance(real_value, list):
                if set(map(str, value)) != set(map(str, real_value)):
                    problems.append(f"{rid}.{key}: 内嵌 {value} != 真实 {real_value}")
            elif not isinstance(value, (list, dict)) and value != real_value:
                problems.append(f"{rid}.{key}: 内嵌 {value!r} != 真实 {real_value!r}")
    for key, value in DEFAULT_META.items():
        real_value = real.meta.get(key)
        if isinstance(value, list) and set(map(str, value)) != set(map(str, real_value or [])):
            problems.append(f"[meta].{key} 与真实 spec 不一致（真实 {real_value}）")
        elif not isinstance(value, (list, dict)) and value != real_value:
            problems.append(f"[meta].{key}: 内嵌 {value!r} != 真实 {real_value!r}")
    try:
        real.check_registry_consistency(REGISTRY)
    except Exception as exc:  # noqa: BLE001
        problems.append(f"check_registry_consistency 失败: {exc}")
    problems.extend(verify_pattern_reachability(real))
    problems.extend(verify_attempt_counting())
    return problems


def verify_attempt_counting() -> list[str]:
    """GOV-001 计次语义（runner 级行为，规则夹具盖不到，因此直接断言）。

    要求：**同一修订连续失败才累加**；修订变化即从 1 重新开始；通过即归零。
    为什么必须测：原实现"只要失败就 +1"，把"改一版跑一次"也当成重试循环，
    第 4 次直接 HALTED —— 门禁把自己卡死（实测踩过）。这条断言就是防线。
    """
    problems: list[str] = []
    task = TaskContract.from_dict({
        "id": "SELFTEST-ATTEMPTS", "title": "计次语义", "owner": "agent",
        "allow": ["**/*"], "deny": [], "attempts": 0,
    })

    def expect(label: str, want: int, got: int) -> None:
        if want != got:
            problems.append(f"计次语义错误：{label} 期望 {want}，实际 {got}")

    task.register_attempt("block", "rev-1")
    expect("同修订第 1 次失败", 1, task.attempts)
    task.register_attempt("block", "rev-1")
    expect("同修订第 2 次失败", 2, task.attempts)
    task.register_attempt("block", "rev-2")
    expect("换修订后再失败（应重新从 1 开始）", 1, task.attempts)
    task.register_attempt("pass", "rev-2")
    expect("通过后归零", 0, task.attempts)
    if task.last_failed_revision:
        problems.append("计次语义错误：通过后 last_failed_revision 应清空")
    return problems


def verify_pattern_reachability(real: Spec) -> list[str]:
    """受保护/禁止模式必须至少能命中仓库里的一个受版本控制的文件。

    为什么必须查：模式写错（例如尾随 `/`、拼错目录名）不会报错，只会**永远不触发** ——
    实测 `protected_paths` 里的 `scripts/gate/`、`.github/workflows/` 就是死规则，
    把引擎改成恒 PASS、把 CI 文件删掉都不会被拦。这类"看起来有规则、实际没有"的
    沉默失效，比规则报错危险得多。
    """
    from gate.context import glob_match

    repo = Path(__file__).resolve().parent.parent.parent
    try:
        proc = subprocess.run(["git", "ls-files"], cwd=str(repo),
                              capture_output=True, text=True, timeout=30)
        tracked = [ln.strip().replace("\\", "/") for ln in proc.stdout.splitlines() if ln.strip()]
    except (OSError, subprocess.SubprocessError):
        return ["无法列出仓库文件（git ls-files 失败），跳过模式可达性检查"]

    problems: list[str] = []
    patterns: list[tuple[str, str]] = [(p, "protected_paths") for p in real.protected_paths]
    for rid, rule in real.rules.items():
        for key in ("default_deny", "deny"):
            for pat in (rule.get(key) or []):
                patterns.append((str(pat), f"{rid}.{key}"))
    for pat, where in patterns:
        if any(glob_match(f, pat) for f in tracked):
            continue
        problems.append(
            f"{where} 的模式 {pat!r} 命中不到任何受版本控制的文件（死规则：永远不触发）"
        )
    return problems


# --------------------------------------------------------------------------- #
# 夹具基线重算（显式请求才写盘）
# --------------------------------------------------------------------------- #
def update_fixture_baselines() -> list[str]:
    """重算 _clean 夹具的基线（gate/baseline.json 与 gate/protected_tests.json）。"""
    from gate.checks import non_whitespace_chars
    from gate.checks.check_tests import build_baseline

    messages: list[str] = []
    clean = FIXTURES_DIR / CLEAN_FIXTURE
    case = json.loads((clean / "case.json").read_text(encoding="utf-8"))

    with tempfile.TemporaryDirectory(prefix="gate-fixture-") as tmp:
        work = Path(tmp)
        copy_fixture_tree(case, work)
        ctx, _ = make_context(work, case, "DOC-001")

        doc_chars: dict[str, int] = {}
        for rid in ("DOC-001",):
            rule = ctx.spec.rule(rid)
            names = list((rule.get("budgets") or {}).keys())
            names += list(rule.get("grandfathered") or [])
            for name in dict.fromkeys(names):
                text = ctx.read(name)
                if text is not None:
                    doc_chars[name] = non_whitespace_chars(text)
        baseline = {
            "generated_at": time.strftime("%Y-%m-%dT%H:%M:%S%z"),
            "generator": "scripts/gate/selftest.py::update_fixture_baselines",
            "doc_chars": doc_chars,
            "warnings": {"g++": 0, "clang++": 0},
        }
        _write(clean / "baseline" / "baseline.json",
               json.dumps(baseline, ensure_ascii=False, indent=2, sort_keys=True) + "\n")
        messages.append(f"已写入 {clean / 'baseline' / 'baseline.json'}")

        protected = build_baseline(ctx)
        protected["generated_at"] = time.strftime("%Y-%m-%dT%H:%M:%S%z")
        protected["generator"] = "scripts/gate/selftest.py::update_fixture_baselines"
        _write(clean / "baseline" / "protected_tests.json",
               json.dumps(protected, ensure_ascii=False, indent=2, sort_keys=True) + "\n")
        messages.append(
            f"已写入 {clean / 'baseline' / 'protected_tests.json'}（{protected['file_count']} 个文件）"
        )
    return messages


# --------------------------------------------------------------------------- #
# 主流程
# --------------------------------------------------------------------------- #
def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="门禁规则负向回归自测")
    parser.add_argument("--list", action="store_true", help="只列出场景，不执行")
    parser.add_argument("--only", action="append", default=[], help="只跑指定规则 ID（可重复）")
    parser.add_argument("--update-fixture-baselines", action="store_true",
                        help="重算 _clean 夹具基线（会写 gate/fixtures/_clean/baseline/）")
    parser.add_argument("--baselines-only", action="store_true",
                        help="只重算夹具基线，不执行场景（配合 --update-fixture-baselines）")
    parser.add_argument("--verbose", action="store_true", help="打印每条 finding 明细")
    parser.add_argument("--strict", action="store_true",
                        help="环境缺失导致的 SKIP 视为失败（CI 用：CI 必须装齐工具做完整验证）")
    args = parser.parse_args(argv)

    if args.update_fixture_baselines:
        for msg in update_fixture_baselines():
            print(msg)
        if args.baselines_only:
            return 0

    drift = verify_spec_drift()
    if drift:
        print("[FAIL] 内嵌自测规格与 gate/spec.toml 不一致：")
        for item in drift:
            print(f"       - {item}")
        return 1

    scenarios, problems = discover_scenarios()
    if problems:
        for item in problems:
            print(f"[FAIL] {item}")
        return 1

    if args.only:
        wanted = set(args.only)
        scenarios = [s for s in scenarios if s.rule_id in wanted]

    if args.list:
        for s in scenarios:
            print(f"{s.id}\t{s.kind}\t{s.title}")
        return 0

    results: list[tuple[str, bool, str]] = []
    clean_scenarios = [s for s in scenarios if s.kind == "clean"]
    negative_scenarios = [s for s in scenarios if s.kind != "clean"]
    total_rules = len(DEFAULT_RULES)
    rule_fail: dict[str, int] = {}
    clean_ok = True
    clean_detail: list[tuple[str, str]] = []

    skipped: list[tuple[str, str]] = []
    for scenario in negative_scenarios:
        ok, detail, err = _run_one(scenario, args.verbose)
        text = err or detail
        if ok and text.startswith("SKIP:"):
            skipped.append((scenario.id, text))
            if getattr(args, "strict", False):
                ok = False
                text = f"{text}（--strict：SKIP 视为失败）"
        results.append((scenario.id, ok, text))
        if not ok:
            rule_fail[scenario.rule_id] = rule_fail.get(scenario.rule_id, 0) + 1
    passed_rules = {rid for rid in DEFAULT_RULES if rid not in rule_fail}

    for scenario in clean_scenarios:
        fakes = _install_fakes(list(scenario.case.get("fake_modules") or []))
        try:
            with tempfile.TemporaryDirectory(prefix="gate-selftest-") as tmp:
                work = Path(tmp)
                copy_fixture_tree(scenario.case, work)
                for rule_id in sorted(DEFAULT_RULES):
                    ctx, rule = make_context(work, scenario.case, rule_id)
                    findings, err = run_check(ctx, rule)
                    if err is not None:
                        clean_ok = False
                        clean_detail.append((rule_id, f"执行异常: {err}"))
                        continue
                    bad = [f for f in findings if f.status in scenario.expect_none]
                    if bad:
                        clean_ok = False
                        clean_detail.append(
                            (rule_id, f"误报 {bad[0].status.value}: {bad[0].subject} — {bad[0].message[:160]}")
                        )
        finally:
            _restore_fakes(fakes)

    print("=" * 78)
    print("门禁规则自测（负向：该红时必须红；正向：不许误报）")
    print("=" * 78)
    for rule_id, ok, detail in results:
        tag = "OK" if ok else "FAIL"
        if ok and detail.startswith("SKIP:"):
            tag = "SKIP"
        print(f"[{tag}] {rule_id:<9} {detail}")

    if clean_scenarios:
        label = "干净场景"
        if clean_ok:
            print(f"[OK] {label:<9} 16 条规则全部无 block/manual/halted/error（不误报）")
        else:
            print(f"[FAIL] {label:<9} 出现误报：")
            for rule_id, detail in clean_detail:
                print(f"       - {rule_id}: {detail}")

    passed = len(passed_rules) + (1 if clean_ok and clean_scenarios else 0)
    total = total_rules + (1 if clean_scenarios else 0)
    verdict_ok = passed_rules == set(DEFAULT_RULES) and clean_ok
    if skipped:
        print("-" * 78)
        print(f"⚠ 有 {len(skipped)} 条规则因本机缺少工具而未能验证（不是通过）：")
        for rule_id, detail in skipped:
            print(f"       - {rule_id}: {detail}")
        print("       CI 必须安装齐全工具并用 --strict 运行，保证零 SKIP。")
    print("-" * 78)
    print(f"{passed}/{total} passed（规则 {len(passed_rules)}/{total_rules}，正向 {'OK' if clean_ok else 'FAIL'}）")
    missing = sorted(set(DEFAULT_RULES) - passed_rules)
    if missing:
        print(f"未通过的规则: {missing}")
    return 0 if verdict_ok else 1


def _run_one(scenario: Scenario, verbose: bool) -> tuple[bool, str, str | None]:
    """跑一个负向场景：只执行该规则（可选再用 also_run 补充断言），避免无关 check 干扰。

    环境不满足（例如夹具要求 cppcheck 但本机没有）时返回 SKIP 而不是 FAIL：
    "无法验证"与"规则失效"是两件事，混为一谈会让门禁自测在缺工具的机器上永远红。
    但 SKIP 也绝不是通过 —— 调用方会单独统计，CI 用 --strict 把 SKIP 判为失败。
    """
    required = [str(tool) for tool in (scenario.case.get("requires") or [])]
    missing_tools = [tool for tool in required if shutil.which(tool) is None]
    if missing_tools:
        return (
            True,
            f"SKIP: 本机缺少 {'/'.join(missing_tools)}，该规则的负向夹具无法在此环境验证",
            None,
        )
    fakes = _install_fakes(list(scenario.case.get("fake_modules") or []))
    try:
        with tempfile.TemporaryDirectory(prefix=f"gate-selftest-{scenario.rule_id}-") as tmp:
            work = Path(tmp)
            copy_fixture_tree(scenario.case, work)
            ctx, rule = make_context(work, scenario.case, scenario.rule_id)
            findings, err = run_check(ctx, rule)
            if err is not None:
                return False, "", f"执行失败: {err}"
            if verbose:
                for f in findings:
                    print(f"    [{scenario.id}] {f.status.value} {f.subject}: {f.message}")
            ok, detail = evaluate(scenario, findings)
            floor = scenario.case.get("min_findings")
            if ok and floor is not None:
                n = len([f for f in findings if f.status in scenario.expect])
                if n < int(floor):
                    return False, "", f"产出的失败类 finding 只有 {n} 条 < 夹具要求 {floor}"
            for other in scenario.case.get("also_run") or []:
                ctx2, rule2 = make_context(work, scenario.case, str(other))
                more, err2 = run_check(ctx2, rule2)
                if err2 is not None:
                    return False, "", f"附加规则 {other} 执行失败: {err2}"
                bad = [f for f in more if f.status in scenario.expect_none]
                if bad:
                    return False, "", f"附加规则 {other} 误报 {bad[0].status.value}: {bad[0].message[:120]}"
            return ok, detail, None
    finally:
        _restore_fakes(fakes)


if __name__ == "__main__":
    raise SystemExit(main())

