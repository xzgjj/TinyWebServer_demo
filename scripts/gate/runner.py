"""门禁引擎：把「规格 + 事实 + 例外」编排成一次可复核的判定。

为什么存在：规则只回答"这一条过不过"，门禁要回答"这次改动能不能进主干"。
这需要在规则之外固定四件事：执行顺序（= spec 声明顺序，并发也不乱）、异常兜底
（检查崩了算 ERROR 而不是消失）、例外边界（只有人批准过的才算）、重试上限。
语义：非 waived 的 BLOCK/MANUAL/ERROR -> blocked(1)；HALTED -> halted(2)；仅 WARN -> pass(0)。
计次：attempts 为连续失败数，PASS 归零；attempts > max_gate_attempts 即 HALTED（第 max+1 次失败）。
"""
from __future__ import annotations

import concurrent.futures
import platform
import shutil
import hashlib
import subprocess
import sys
import time
import traceback
from pathlib import Path
from typing import Any, Callable
from . import ledger as ledger_mod
from . import report as report_mod
from . import waivers as waivers_mod
from .context import Context, build_context, match_any
from .model import ExitCode, Finding, GateReport, RuleResult, Status, new_run_id
from .spec import Rule, Spec, SpecError
from .task import TaskContract, TaskError, now_iso
from .waivers import Waiver

#: 需要编译/静态分析的重规则（--skip-heavy 时跳过；跳过 ≠ 通过，报告里标 warn）
HEAVY_CHECKS = frozenset({"lint", "complexity", "problems_panel", "test_coverage"})
DEFAULT_WORKERS = 4


class EnvError(Exception):
    """运行环境不满足门禁的前提（仓库不存在、任务契约损坏等）-> 退出码 4。"""


# --------------------------------------------------------------------------- #
# 注册表装载（checks 包由另一模块提供）
# --------------------------------------------------------------------------- #
def load_registry() -> dict[str, Callable[[Context, Rule], list[Finding]]]:
    """装载 check 注册表；checks 包缺失/半成品时给出可执行的报错（SpecError）。

    规格与实现必须双向一致：spec 声明了却没实现 = 规则形同虚设；
    实现了却没被引用 = 有人在偷偷加规则。两种都由
    Spec.check_registry_consistency() 兜住。
    """
    try:
        from .checks import load_registry as _load  # 延迟导入：允许 checks 尚未落地
    except ImportError as exc:
        raise SpecError(
            "无法导入 scripts/gate/checks 包（约定的接口：checks/__init__.py 暴露 "
            f"REGISTRY 与 load_registry()）。原始错误: {type(exc).__name__}: {exc}"
        ) from exc
    try:
        registry = _load()
    except Exception as exc:
        raise SpecError(f"checks.load_registry() 执行失败: {type(exc).__name__}: {exc}") from exc
    if not isinstance(registry, dict):
        raise SpecError(f"checks.load_registry() 必须返回 dict，实际返回 {type(registry).__name__}")
    bad = [k for k, v in registry.items() if not callable(v)]
    if bad:
        raise SpecError(f"以下 check 不是可调用对象: {sorted(bad)}")
    return registry


# --------------------------------------------------------------------------- #
# 小工具
# --------------------------------------------------------------------------- #
def _rule_param(spec: Spec, rule_id: str, key: str, default: Any = None) -> Any:
    try:
        return spec.rule(rule_id).get(key, default)
    except SpecError:
        return default


def _jsonable(value: Any) -> Any:
    """把 options 之类的自由输入压成可 JSON 序列化的形态（报告要落盘）。"""
    if value is None or isinstance(value, (str, int, float, bool)):
        return value
    if isinstance(value, Path):
        return value.as_posix()
    if isinstance(value, (list, tuple, set)):
        return [_jsonable(v) for v in value]
    if isinstance(value, dict):
        return {str(k): _jsonable(v) for k, v in value.items()}
    return str(value)


def spec_path_for(root: Path, spec_path: str | Path | None) -> Path:
    p = Path(spec_path) if spec_path else Path("gate/spec.toml")
    return p if p.is_absolute() else Path(root) / p


def task_path_for(root: Path, spec: Spec, task_id: str) -> Path:
    d = spec.path_of("task_dir", "gate/tasks")
    d = d if d.is_absolute() else Path(root) / d
    return d / f"{task_id}.json"


def exit_code_for(verdict: Status) -> int:
    if verdict is Status.HALTED:
        return ExitCode.HALTED
    if verdict in (Status.BLOCK, Status.MANUAL, Status.ERROR):
        return ExitCode.BLOCKED
    return ExitCode.PASS


def aggregate_verdict(results: list[RuleResult]) -> Status:
    """按 finding 状态汇总（WAIVED 不算失败；WARN-only 仍算 pass）。"""
    statuses = [r.status for r in results]
    if Status.HALTED in statuses:
        return Status.HALTED
    if any(s in (Status.BLOCK, Status.MANUAL, Status.ERROR) for s in statuses):
        return Status.BLOCK
    return Status.PASS


# --------------------------------------------------------------------------- #
# 任务契约
# --------------------------------------------------------------------------- #
def load_task(root: Path, spec: Spec, task_id: str | None) -> tuple[TaskContract | None, Path | None]:
    """加载任务契约；文件不存在 -> (None, path)，交给 SCOPE-001 自己阻断。"""
    if not task_id:
        return None, None
    path = task_path_for(root, spec, task_id)
    if not path.is_file():
        return None, path
    try:
        return TaskContract.load(path), path
    except TaskError as exc:
        raise EnvError(
            f"任务契约损坏: {path.as_posix()}（{exc}）；请修正或重新 declare，"
            "门禁拒绝在契约不可读的情况下给出结论"
        ) from exc


# --------------------------------------------------------------------------- #
# 环境摘要
# --------------------------------------------------------------------------- #
def collect_environment(root: Path, spec: Spec, ctx: Context, options: dict[str, Any]) -> dict[str, Any]:
    facts = ctx.git
    compilers = options.get("compilers") or _rule_param(spec, "QUAL-001", "compilers", []) or []
    env: dict[str, Any] = {
        "python": platform.python_version(),
        "executable": sys.executable,
        "platform": f"{platform.system()} {platform.release()} ({platform.machine()})",
        "cwd": Path.cwd().as_posix(),
        "root": root.as_posix(),
        "spec_version": spec.meta.get("spec_version", ""),
        "project": spec.meta.get("project", ""),
        "git": {
            "available": bool(facts.commit),
            "mode": facts.mode,
            "ref_range": facts.ref_range,
            "commit": facts.commit,
            "branch": facts.branch,
            "author": facts.author_name,
            "changed_files": len(facts.changed),
            "affected_files": len(ctx.affected()),
            "lines_changed": facts.lines_changed(),
        },
        "compilers": {str(c): shutil.which(str(c)) for c in compilers},
        "std": options.get("std") or _rule_param(spec, "QUAL-001", "standard", ""),
        "compile_commands": (root / "compile_commands.json").is_file(),
        "options": _jsonable(options),
        "skipped_rules": [],
        "applied_waivers": [],
        "check_errors": {},
        "halt": None,
        "notes": ctx.notes,          # 与 ctx 共享同一 list，运行中追加即时可见
        "errors": [],
    }
    if not facts.commit:
        env["notes"].append(
            "⚠ git 事实不可用（非 git 工作区或缺 git）：改动集为空，SCOPE/预算类规则的结论不可信"
        )
    if facts.mode == "range" and facts.ref_range:
        env["notes"].append(f"判定区间：{facts.ref_range}（CI 模式）")
    if not env["compilers"]:
        env["notes"].append("未配置编译器（QUAL-001 的 compilers 为空）")
    return env


# --------------------------------------------------------------------------- #
# 单条规则执行
# --------------------------------------------------------------------------- #
def _error_result(rule: Rule, message: str) -> RuleResult:
    return RuleResult(rule_id=rule.id, title=rule.title, mode=rule.mode, findings=[], error=message)


def _coerce_findings(rule: Rule, raw: Any, ctx: Context) -> tuple[list[Finding], str | None]:
    """把 check 的返回值规整成 list[Finding]；不合约定直接算规则出错（不静默放过）。"""
    if raw is None:
        return [], f"check {rule.check} 返回 None；约定必须返回 list[Finding]"
    if isinstance(raw, Finding):
        return [], f"check {rule.check} 返回了单个 Finding；约定必须返回 list[Finding]"
    if not isinstance(raw, (list, tuple)):
        return [], f"check {rule.check} 返回 {type(raw).__name__}；约定必须返回 list[Finding]"
    out: list[Finding] = []
    for idx, item in enumerate(raw, 1):
        if not isinstance(item, Finding):
            return [], f"check {rule.check} 第 {idx} 个元素是 {type(item).__name__}；必须是 Finding"
        if not item.rule:
            item.rule = rule.id
        elif item.rule != rule.id:
            ctx.log(f"{rule.id}：check 返回的 finding.rule={item.rule} 与规则不一致，已按 {rule.id} 归档")
            item.rule = rule.id
        out.append(item)
    return out, None


def _run_one(
    rule: Rule,
    ctx: Context,
    registry: dict[str, Callable[[Context, Rule], list[Finding]]],
    skip_heavy: bool,
) -> tuple[RuleResult, str | None]:
    """执行一条规则，返回 (结果, 可选 traceback)。任何异常都转成 RuleResult.error。"""
    if skip_heavy and rule.check in HEAVY_CHECKS:
        f = Finding(
            rule=rule.id,
            status=Status.WARN,
            subject=f"{rule.id} · {rule.check}",
            message=f"SKIPPED：--skip-heavy 跳过了需要编译/静态分析的重规则（{rule.check}）。跳过不等于通过。",
            observed="skipped",
            threshold="必须执行",
            level="warning",
            hint=f"去掉 --skip-heavy 重跑，或在完整 CI 任务中执行 {rule.id}。",
        )
        return RuleResult(rule_id=rule.id, title=rule.title, mode=rule.mode, findings=[f], duration_ms=0), None

    fn = registry.get(rule.check)
    if fn is None:
        return _error_result(rule, f"注册表中不存在 check={rule.check}（规格与实现不一致）"), None

    t0 = time.perf_counter()
    try:
        raw = fn(ctx, rule)
    except Exception as exc:                      # 检查崩了必须显式变成 ERROR，而不是"没发现"
        ms = int((time.perf_counter() - t0) * 1000)
        tb = traceback.format_exc()
        ctx.log(f"{rule.id}：check({rule.check}) 抛异常 {type(exc).__name__}: {exc}")
        return (
            RuleResult(
                rule_id=rule.id,
                title=rule.title,
                mode=rule.mode,
                duration_ms=ms,
                error=f"{type(exc).__name__}: {exc}",
            ),
            tb,
        )
    ms = int((time.perf_counter() - t0) * 1000)
    findings, err = _coerce_findings(rule, raw, ctx)
    if err:
        return _error_result(rule, err), None
    return RuleResult(rule_id=rule.id, title=rule.title, mode=rule.mode, findings=findings, duration_ms=ms), None


def run_rules(
    spec: Spec,
    ctx: Context,
    registry: dict[str, Callable[[Context, Rule], list[Finding]]],
    options: dict[str, Any],
) -> tuple[list[RuleResult], dict[str, str]]:
    """按 spec 声明顺序执行全部规则（可并发，但结果顺序永远与声明一致）。"""
    rules = list(spec.rules.values())            # tomllib 保持文档顺序 = 声明顺序
    skip_heavy = bool(options.get("skip_heavy"))
    workers = max(1, int(options.get("workers") or DEFAULT_WORKERS))
    serial = bool(options.get("serial")) or workers == 1 or len(rules) <= 1

    if serial:
        pairs = [_run_one(r, ctx, registry, skip_heavy) for r in rules]
    else:
        with concurrent.futures.ThreadPoolExecutor(max_workers=workers, thread_name_prefix="gate") as ex:
            futures = [(r, ex.submit(_run_one, r, ctx, registry, skip_heavy)) for r in rules]
            pairs = []
            for rule, fut in futures:            # 按声明顺序回收，保证报告稳定可对比
                try:
                    pairs.append(fut.result())
                except Exception as exc:         # _run_one 内部已兜底，这里只是最后一道
                    pairs.append((_error_result(rule, f"{type(exc).__name__}: {exc}"), traceback.format_exc()))

    results: list[RuleResult] = []
    tracebacks: dict[str, str] = {}
    for res, tb in pairs:
        results.append(res)
        if tb:
            tracebacks[res.rule_id] = tb
    return results, tracebacks


# --------------------------------------------------------------------------- #
# 例外
# --------------------------------------------------------------------------- #
def _finding_paths(finding: Finding) -> list[str]:
    """从 finding 中抽出可用于 scope 匹配的路径。

    subject 可能是 "src/a.cpp:12"、"(diff 行数)"、"(任务契约)" 等；只有像路径的才参与匹配。
    """
    out: list[str] = []
    subject = str(getattr(finding, "subject", "") or "")
    token = subject.split(":")[0].strip()
    if token and "/" in token and not token.startswith("("):
        out.append(token.replace("\\", "/"))
    ev = getattr(finding, "evidence", None)
    if ev:
        out.append(str(ev).replace("\\", "/"))
    return out


def apply_waivers(results: list[RuleResult], active: list[Waiver],
                  rule_level_allowed: set[str] | None = None) -> list[Waiver]:
    """把已批准的例外落到 findings 上；返回实际生效的例外列表。

    只有 BLOCK / MANUAL 可以被例外覆盖：
      * HALTED（重试超限）——只能由人执行 gate reset，不能被例外绕过；
      * ERROR（检查自身出错）——环境问题必须修好，掩盖它等于掩盖门禁失效。

    **scope 语义（重要）**：waiver 的 scope_files 限定"豁免哪些文件"，不是"这条规则要不要生效"。
      实测过一个可绕过路径：一条 scope=[src/a.cpp] 的 SCOPE-001 例外，把同一规则下
      "LICENSE 被篡改（default_deny）"的越界项一并豁免了（exit 0）。因此：
        - scope 非空：只豁免路径命中 scope 的 finding，其余保持阻断；
        - scope 为空：只有 rule_level_allowed 白名单里的规则（默认仅 CI-001）允许，
          其它规则的空 scope 例外不生效（报告里会说明原因）。
    """
    allowed_rule_level = rule_level_allowed or set()
    applied: list[Waiver] = []
    for r in results:
        candidates = [w for w in active if w.rule == r.rule_id]
        if not candidates:
            continue
        hit = 0
        for f in r.findings:
            if f.status not in (Status.BLOCK, Status.MANUAL):
                continue
            paths = _finding_paths(f)
            effective: Waiver | None = None
            for w in candidates:
                if not w.scope_files:
                    if r.rule_id in allowed_rule_level:
                        effective = w
                        break
                    continue
                if paths and any(match_any(p, w.scope_files) for p in paths):
                    effective = w
                    break
            if effective is None:
                continue
            f.status = Status.WAIVED
            f.hint = (f.hint or "") + (
                f" [已由 {effective.waiver_id} 豁免；批准人={effective.approved_by}]"
            )
            hit += 1
            if effective not in applied:
                applied.append(effective)
            r.waived_by = effective.approved_by
            r.waiver_id = effective.waiver_id
    return applied


def _halt_info(spec: Spec, task: TaskContract | None, reason: str) -> dict[str, Any]:
    m = spec.max_gate_attempts
    attempts = task.attempts if task else 0
    tid = task.id if task else "TASK-ID"
    return {
        "max_attempts": m,
        "attempts": attempts,
        "reason": reason,
        "semantics": (
            f"语义：允许连续失败 {m} 次（判定式为 attempts > max_gate_attempts）；"
            f"即第 {m} 次失败仍属允许范围，第 {m + 1} 次运行仍失败即 HALTED；一旦 PASS 计数自动归零。"
        ),
        "message": f"任务 {tid} 已连续失败 {attempts} 次，超过上限 {m} 次，禁止继续自动重试；需人工 `gate reset` 才能解锁。",
        "hint": f'python scripts/gate/cli.py reset --task {tid} --reason "说明为什么允许再试"',
    }


# --------------------------------------------------------------------------- #
# 主流程
# --------------------------------------------------------------------------- #
#: 门禁自己的记账文件：它们会**因为跑门禁而改变**，不能算进"修订指纹"，
#: 否则每次运行指纹都不同 -> 计数永远归 1 -> 重试上限形同虚设（实测踩过）。
_BOOKKEEPING = ("gate/tasks/", "gate/ledger.jsonl", "gate/waivers.jsonl",
                "local/", "build/", "audit_report.md")


def _is_bookkeeping(path: str) -> bool:
    return any(path.startswith(prefix) for prefix in _BOOKKEEPING)


def revision_fingerprint(ctx: Context) -> str:
    """本次被判定内容的修订指纹：提交 + 规格 + 工作区改动（排除门禁自身记账文件）。

    用途：GOV-001 的"同一修订连续失败才计次"。只要改动了代码/契约/规格，
    指纹就变，计数从 1 重新开始 —— 这样"改一版跑一次"是正常迭代，
    而"原样重跑"才会被判定为原地打转。
    """
    status = sorted(f"{k}:{v}" for k, v in (ctx.git.status_map or {}).items()
                    if not _is_bookkeeping(k))
    parts = [str(ctx.git.commit or ""), str(ctx.spec.spec_hash or ""), "|".join(status)]
    excludes = [f":(exclude){p.rstrip('/')}/**" if p.endswith("/") else f":(exclude){p}"
                for p in _BOOKKEEPING]
    try:
        diff = subprocess.run(
            ["git", "diff", "HEAD", "--", ".", *excludes],
            cwd=str(ctx.root), capture_output=True, text=True, timeout=30,
        ).stdout
        parts.append(hashlib.sha1(diff.encode("utf-8", "replace")).hexdigest()[:12])
    except (OSError, subprocess.SubprocessError):
        parts.append("nodiff")
    return hashlib.sha1("|".join(parts).encode("utf-8", "replace")).hexdigest()[:16]


def run_gate(
    root: Path,
    spec_path: Path | None = None,
    task_id: str | None = None,
    options: dict[str, Any] | None = None,
) -> GateReport:
    """跑一次门禁并返回 GateReport（同时落盘报告与账本，除非 options 关闭）。

    options：base(A..B) / serial / verbose / skip_heavy / report_dir / compilers / std
             / no_ledger / write_reports / workers
    """
    opts = dict(options or {})
    root_p = Path(root).resolve()
    if not root_p.is_dir():
        raise EnvError(f"仓库根目录不存在: {root_p}")

    t_start = time.perf_counter()
    started_at = now_iso()

    spec = Spec.load(spec_path_for(root_p, spec_path))          # 失败 -> SpecError -> exit 3
    registry = load_registry()
    spec.check_registry_consistency(registry)

    task, task_file = load_task(root_p, spec, task_id)
    attempt = (task.attempts if task else 0) + 1
    name = task_id or (task.id if task else "adhoc")
    run_id = new_run_id(name, attempt)

    ctx = build_context(root_p, spec, task, run_id, opts)
    env = collect_environment(root_p, spec, ctx, opts)
    if task_file and task is None:
        env["notes"].append(f"未找到任务契约 {task_file.as_posix()}，task=None（由 SCOPE-001 决定是否阻断）")

    # 账本要先存在：GOV-002 的 audit 检查会在运行过程中读它，
    # "首次运行还没有账本"不该被误判成"账本被删了"。
    try:
        ledger_mod.ensure(root_p, spec)
    except OSError as exc:
        env["errors"].append(f"账本不可写：{exc}（GOV-002 会因此阻断）")

    results, tracebacks = run_rules(spec, ctx, registry, opts)
    env["check_errors"] = tracebacks
    env["skipped_rules"] = [
        r.rule_id for r in results if any(f.observed == "skipped" for f in r.findings)
    ]
    if opts.get("verbose"):
        env["notes"].append(
            "逐规则耗时(ms)：" + "，".join(f"{r.rule_id}={r.duration_ms}" for r in results)
        )

    active: list[Waiver] = []
    try:
        active = waivers_mod.active_waivers(
            root_p, spec, ctx.git.commit, ctx.git.author_name, affected=ctx.git.changed
        )
    except Exception as exc:  # noqa: BLE001 - 例外账本读不了就按"无例外"处理（只会更严），
        # 且必须留痕；账本真的坏了由 CI-001 的 rule_guard 检查独立阻断。
        env["notes"].append(
            f"例外账本读取失败（{type(exc).__name__}: {exc}）：本次按无例外处理，判定只会更严"
        )
    applied = apply_waivers(results, active,
                            rule_level_allowed=set(spec.meta.get("rule_level_waiver_allowed") or []))
    env["applied_waivers"] = [w.to_dict() for w in applied]
    if active and not applied:
        env["notes"].append(f"有 {len(active)} 条生效例外，但本次没有任何规则命中它们")

    # 棘轮候选值：判定器把"本次实测值"发布到 ctx.cache，这里统一导出到报告，
    # 供 `gate run --update-baseline` 重算 gate/baseline.json（棘轮唯一受支持的重建入口）。
    env["baseline_candidates"] = {
        "doc_chars": ctx.cache.get("baseline_doc_chars") or {},
        "warnings": ctx.cache.get("baseline_warnings") or {},
        "complexity": ctx.cache.get("baseline_complexity") or {},
        "cppcheck": ctx.cache.get("baseline_cppcheck"),
    }

    verdict = aggregate_verdict(results)
    halt: dict[str, Any] | None = None
    if verdict is Status.HALTED:
        halt = _halt_info(spec, task, "rule_halted")

    # 尝试计数：register_attempt 后超出上限 -> 整体 HALTED(2)
    if task is not None:
        task.register_attempt(verdict.value, revision_fingerprint(ctx))
        if verdict is not Status.PASS and task.attempts > spec.max_gate_attempts:
            verdict = Status.HALTED
            task.state = "HALTED"
            halt = _halt_info(spec, task, "attempts_exceeded")
        if task_file:
            try:
                task.save(task_file)
            except (OSError, TaskError) as exc:
                env["errors"].append(f"任务契约回写失败：{exc}")
    env["halt"] = halt

    report = GateReport(
        run_id=run_id,
        task=name,
        attempt=attempt,
        commit=ctx.git.commit,
        branch=ctx.git.branch,
        started_at=started_at,
        duration_s=time.perf_counter() - t_start,
        verdict=verdict,
        exit_code=exit_code_for(verdict),
        results=results,
        environment=env,
        spec_hash=spec.spec_hash,
        report_dir=str(opts.get("report_dir") or ""),
    )

    # 先定产物目录（账本记录里要带上它），再写账本，最后落盘报告。
    try:
        d = report_mod.run_dir_for(root_p, report, spec)
        try:
            report.report_dir = d.resolve().relative_to(root_p.resolve()).as_posix()
        except ValueError:
            report.report_dir = d.as_posix()
    except (OSError, ValueError) as exc:
        env["errors"].append(f"产物目录计算失败：{exc}")

    if not opts.get("no_ledger"):
        try:
            ledger_mod.append_run(root_p, report, spec)
        except (OSError, SpecError) as exc:
            env["errors"].append(f"账本写入失败：{exc}（GOV-002 留痕缺失）")

    if opts.get("write_reports", True):
        try:
            report_mod.write_reports(root_p, report, spec)
        except OSError as exc:
            env["errors"].append(f"报告写入失败：{exc}")
            print(f"⚠ 报告写入失败（不影响判定）：{exc}", file=sys.stderr)

    # 不再更新 duration_s：保持内存报告与 report.json 里的数字一致（耗时至判定为止）。
    return report
