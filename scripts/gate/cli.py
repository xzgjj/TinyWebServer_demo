"""命令行入口：门禁唯一的操作面（人类与 CI 都从这里进）。

为什么存在：门禁的每个动作都要能被同一个人重复执行，所以能力全部收敛成子命令
（run/declare/status/explain/report/exception/reset/selftest），异常一律转成可读信息。
退出码取自 model.ExitCode：0 通过 / 1 阻断 / 2 已停止 / 3 规格错误 / 4 环境错误。
main() 可被 tools.py 直接调用；也支持 `python scripts/gate/cli.py <cmd>`。
"""
from __future__ import annotations

import argparse
import inspect
import json
import os
import sys
import time
import traceback
from pathlib import Path

_REPO_ROOT = Path(__file__).resolve().parents[2]

# 允许把本文件当脚本直接执行：把仓库根塞进 sys.path，使 "scripts.gate.*" 可导入。
if __package__ in (None, ""):
    if str(_REPO_ROOT) not in sys.path:
        sys.path.insert(0, str(_REPO_ROOT))
    __package__ = "scripts.gate"

from scripts.gate import ledger, report as report_mod, runner, waivers
from scripts.gate.context import collect_git, match_any
from scripts.gate.model import STATUS_ICON, ExitCode, Status
from scripts.gate.spec import Spec, SpecError
from scripts.gate.task import TaskContract, TaskError, now_iso
from scripts.gate.waivers import WaiverError

#: 规则修复指引（按 check 归类；只讲"怎么改"，阈值一律以 spec 为准）
FIX_HINTS: dict[str, str] = {
    "doc_budget": "把文档压回预算内；存量文档受棘轮约束只许减不许增；记录/分析类请放到 unlimited_globs 允许的位置。",
    "doc_dup": "删掉重复段落，改成引用（章节号/链接），不要复制粘贴细节。",
    "doc_refs": "修正文档里不存在的路径；或让 CMake 的默认 C++ 标准与 CLAUDE.md 的声明保持一致。",
    "dead_code": "删除未使用的私有字段/静态函数与整段注释代码；确实要用就把调用点接上。",
    "dup_code": "把重复窗口抽成公共函数/模板，消除复制粘贴。",
    "scope": "把改动收进契约 scope.allow；要扩大范围就重新 declare 契约，不要顺手改别的文件。",
    "diff_budget": "拆成多次小提交；确需大改就重新 declare 一个更大预算的契约（由人批准）。",
    "lint": "修掉新增警告；不要用 pragma 静音，除非写明理由并申请例外。",
    "complexity": "拆分函数、降低嵌套（早返回代替深层 if）。",
    "problems_panel": "修好 compile_commands.json 的生成，清掉 clangd/cppcheck 报出的 error。",
    "std_features": "改用白名单内的特性，或用 #if <guard_macro>* 包住交集外特性（宏名见规则 params）。",
    "protected_tests": "测试是受保护资产：恢复被改/被删的用例，或走【非作者】审批的例外。",
    "test_coverage": "为新模块补 test_*.cpp 并在 CMakeLists 注册，确保 smoke 集通过。",
    "rule_guard": "受保护路径的变更必须附一条 approved_by != 变更作者的 CI-001 例外。",
    "retry_cap": "已到重试上限：停下来人工判断，确认后 gate reset，不要靠反复重跑刷次数。",
    "audit": "修好 gate/ledger.jsonl（缺字段/坏行），保证每次运行都留痕。",
}


# --------------------------------------------------------------------------- #
# 基础设施
# --------------------------------------------------------------------------- #
def _setup_stdout() -> None:
    """Windows 控制台默认编码可能装不下 ✓/⊘/⏸ 等符号，统一改成 utf-8 + 替换。"""
    for stream in (sys.stdout, sys.stderr):
        try:
            stream.reconfigure(encoding="utf-8", errors="replace")  # type: ignore[union-attr]
        except (AttributeError, ValueError, OSError):
            pass


def _common_parent() -> argparse.ArgumentParser:
    """--root/--spec 同时挂在主解析器与各子命令上；子命令侧用 SUPPRESS，
    这样 `gate --root X run` 与 `gate run --root X` 都能用且互不覆盖。"""
    common = argparse.ArgumentParser(add_help=False)
    common.add_argument("--root", default=argparse.SUPPRESS, metavar="PATH",
                        help="仓库根目录（默认：本脚本所在仓库根）")
    common.add_argument("--spec", default=argparse.SUPPRESS, metavar="PATH",
                        help="规格文件（默认：<root>/gate/spec.toml）")
    return common


def resolve_root(args: argparse.Namespace) -> Path:
    value = getattr(args, "root", None)
    return Path(value).expanduser().resolve() if value else _REPO_ROOT.resolve()


def resolve_spec(args: argparse.Namespace, root: Path) -> Path:
    value = getattr(args, "spec", None)
    return Path(value).expanduser() if value else root / "gate" / "spec.toml"


def load_spec(root: Path, args: argparse.Namespace) -> Spec:
    return Spec.load(resolve_spec(args, root))          # 失败 -> SpecError -> exit 3


def report_dir_of(root: Path, spec: Spec) -> Path:
    d = spec.path_of("report_dir", "local/gate")
    return d if d.is_absolute() else root / d


def _split_list(value: str | None, sep: str = ",") -> list[str] | None:
    if value is None:
        return None
    return [item.strip() for item in value.replace(";", sep).split(sep) if item.strip()]


def _print(text: str = "") -> None:
    print(text)


# --------------------------------------------------------------------------- #
# 子命令：run
# --------------------------------------------------------------------------- #
def _update_baseline(root: Path, spec: Spec, rep) -> None:
    """用本次运行的实测值重算 gate/baseline.json 的棘轮基线。

    为什么需要它：DOC-001/QUAL-001/QUAL-002/QUAL-003 的棘轮都依赖 baseline.json，
    没有受支持的重建入口，基线缺失时规则只能降级为 WARN（实测过），棘轮等于没有。
    语义保持"棘轮"：只覆盖本次真正测到的段；未测到的段原样保留。
    """
    cand = (rep.environment or {}).get("baseline_candidates") or {}
    path = root / str(spec.meta.get("baseline_path", "gate/baseline.json"))
    data: dict = {}
    if path.is_file():
        try:
            data = json.loads(path.read_text(encoding="utf-8"))
        except json.JSONDecodeError:
            _print(f"⚠ 现有基线不是合法 JSON，将被覆盖: {path}")
    changed: list[str] = []
    if cand.get("doc_chars"):
        data["doc_chars"] = dict(sorted(cand["doc_chars"].items()))
        changed.append(f"doc_chars({len(cand['doc_chars'])})")
    if cand.get("warnings"):
        data["warnings"] = {**{k: v for k, v in (data.get("warnings") or {}).items()
                              if str(k).startswith("_")},
                            **dict(sorted(cand["warnings"].items()))}
        changed.append(f"warnings({len(cand['warnings'])})")
    if cand.get("complexity"):
        data["complexity"] = dict(sorted(cand["complexity"].items()))
        changed.append(f"complexity({len(cand['complexity'])})")
    if isinstance(cand.get("cppcheck"), int):
        data["cppcheck"] = cand["cppcheck"]
        changed.append(f"cppcheck({cand['cppcheck']})")
    if not changed:
        _print("⚠ --update-baseline：本次没有采集到任何可更新的段（可能用了 --skip-heavy）")
        return
    data["generated_at"] = time.strftime("%Y-%m-%dT%H:%M:%S%z")
    data["generator"] = "gate run --update-baseline（实测值重算）"
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(data, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    _print(f"✓ 已更新棘轮基线 {path}：{', '.join(changed)}")
    _print("  注意：baseline.json 属受保护路径，收紧/放宽都需 CI-001 的非作者审批。")


def cmd_run(args: argparse.Namespace) -> int:
    root = resolve_root(args)
    spec_path = resolve_spec(args, root)
    spec = Spec.load(spec_path)
    # `--task auto`（或不传）时取 gate/tasks/ 里最近更新的契约。
    # 为什么需要：CI 不传 --task 会让 SCOPE-001 "无契约即阻断" 每次都红，
    # 门禁随即被当成噪声忽略 —— 那等于没有门禁。
    task_id = args.task
    if not task_id or str(task_id).lower() == "auto":
        picked, _path = _pick_task(root, spec, None)
        task_id = picked.id if picked is not None else None
        if task_id is None:
            _print("⚠ 未找到任何任务契约（gate/tasks/*.json）：本次将按 SCOPE-001 阻断处理")
    options = {
        "base": args.base,
        "serial": bool(args.serial),
        "verbose": bool(args.verbose),
        "skip_heavy": bool(args.skip_heavy),
        "report_dir": args.report_dir,
        "no_ledger": bool(args.no_ledger),
        "write_reports": True,
    }
    rep = runner.run_gate(root, spec_path, task_id, options)
    if getattr(args, "update_baseline", False):
        _update_baseline(root, spec, rep)
    if args.as_json:
        _print(rep.to_json())
    else:
        _print(report_mod.render_console(rep))
    return rep.exit_code


# --------------------------------------------------------------------------- #
# 子命令：declare
# --------------------------------------------------------------------------- #
def cmd_declare(args: argparse.Namespace) -> int:
    root = resolve_root(args)
    spec = load_spec(root, args)
    path = runner.task_path_for(root, spec, args.task)

    if path.is_file():
        task = TaskContract.load(path)              # 合并更新：不丢任何既有字段
        action = "更新"
    else:
        task = TaskContract(id=args.task, created_at=now_iso())
        action = "新建"

    if args.title is not None:
        task.title = args.title
    if args.phase is not None:
        task.phase = args.phase
    if args.owner is not None:
        task.owner = args.owner
    allow = _split_list(args.scope)
    if allow is not None:
        task.allow = allow
    deny = _split_list(args.deny)
    if deny is not None:
        task.deny = deny
    accept = _split_list(args.accept, sep=";")
    if accept is not None:
        task.acceptance = accept
    if args.budget is not None:
        if args.budget <= 0:
            raise TaskError(f"--budget 必须为正整数，当前为 {args.budget}")
        task.max_changed_lines = args.budget
    if args.max_files is not None:
        if args.max_files <= 0:
            raise TaskError(f"--max-files 必须为正整数，当前为 {args.max_files}")
        task.max_changed_files = args.max_files
    if args.audit_tag is not None:
        task.audit_tag = args.audit_tag

    task.save(path)
    _print(f"{STATUS_ICON[Status.PASS]} 已{action}任务契约：{path.as_posix()}（revision={task.revision}）")
    _print(json.dumps(task.to_dict(), ensure_ascii=False, indent=2))
    return ExitCode.PASS


# --------------------------------------------------------------------------- #
# 子命令：status
# --------------------------------------------------------------------------- #
def _pick_task(root: Path, spec: Spec, task_id: str | None) -> tuple[TaskContract | None, Path | None]:
    if task_id:
        path = runner.task_path_for(root, spec, task_id)
        return (TaskContract.load(path) if path.is_file() else None), path
    d = spec.path_of("task_dir", "gate/tasks")
    d = d if d.is_absolute() else root / d
    files = sorted(d.glob("*.json"), key=lambda p: p.stat().st_mtime, reverse=True) if d.is_dir() else []
    if not files:
        return None, None
    try:
        return TaskContract.load(files[0]), files[0]
    except TaskError:
        return None, files[0]


def cmd_status(args: argparse.Namespace) -> int:
    root = resolve_root(args)
    spec = load_spec(root, args)

    task, path = _pick_task(root, spec, args.task)
    _print("== 当前任务契约 ==")
    if task is None:
        _print(f"（无；查找位置：{path.as_posix() if path else '-'}）")
    else:
        _print(f"文件：{path.as_posix() if path else '-'}")
        _print(
            f"id={task.id} state={task.state} attempts={task.attempts}/{spec.max_gate_attempts}"
            f" owner={task.owner} revision={task.revision}"
        )
        _print(f"标题：{task.title or '-'}  阶段：{task.phase or '-'}  审计标签：{task.audit_tag or '-'}")
        _print(f"allow={task.allow or '-'}")
        _print(f"deny={task.deny or '-'}")
        _print(
            f"预算：max_lines={task.max_changed_lines or '（用 spec 默认）'} "
            f"max_files={task.max_changed_files or '（用 spec 默认）'}"
        )
        _print(f"验收：{'; '.join(task.acceptance) or '-'}")

    _print("\n== 最近一次账本记录 ==")
    rec = ledger.last_record(root, spec)
    _print(json.dumps(rec, ensure_ascii=False, indent=2) if rec else "（账本为空）")

    pending = waivers.pending(root, spec)
    _print(f"\n== 待批准例外：{len(pending)} 条 ==")
    for w in pending:
        _print("  " + waivers.describe(w))

    _print("\n== 工作区 ==")
    facts = collect_git(root)
    _print(
        f"commit={facts.commit or '-'} branch={facts.branch or '-'} "
        f"changed={len(facts.changed)} 文件 / {facts.lines_changed()} 行"
    )
    return ExitCode.PASS


# --------------------------------------------------------------------------- #
# 子命令：explain
# --------------------------------------------------------------------------- #
def cmd_explain(args: argparse.Namespace) -> int:
    root = resolve_root(args)
    spec = load_spec(root, args)
    rule = spec.rule(args.rule_id.upper())
    _print(f"# {rule.id} {rule.title}")
    _print(f"模式 mode ：{rule.mode}（block=阻断 / warn=记账 / manual=转人工 / ratchet=棘轮）")
    _print(f"实现 check：{rule.check}")
    _print(f"说明 desc ：{rule.desc}")
    _print("\n## 阈值（全部来自 gate/spec.toml，改动即规则变更）")
    _print(json.dumps(rule.params, ensure_ascii=False, indent=2, default=str))
    _print("\n## 怎么修")
    _print("  " + FIX_HINTS.get(rule.check, "见规则说明；如需细化请在 spec 的 desc 中写清楚。"))
    _print("\n## 怎么申请例外（需要非作者批准）")
    _print(f'  python scripts/gate/cli.py exception request --rule {rule.id} --reason "为什么要越轨"')
    _print(f"  python scripts/gate/cli.py exception approve --id W-xxxx --by {'/'.join(spec.rule_approvers)}")
    active = [w for w in waivers.list_all(root, spec) if w.rule == rule.id]
    if active:
        _print(f"\n## 该规则的历史例外（{len(active)} 条）")
        for w in active:
            _print("  " + waivers.describe(w) + f" 理由={w.reason}")
    return ExitCode.PASS


# --------------------------------------------------------------------------- #
# 子命令：report
# --------------------------------------------------------------------------- #
def cmd_report(args: argparse.Namespace) -> int:
    root = resolve_root(args)
    spec = load_spec(root, args)
    base = report_dir_of(root, spec)

    if args.run in ("last", "", None):
        dirs = [d for d in base.glob("*") if (d / "report.md").is_file()] if base.is_dir() else []
        if not dirs:
            _print(f"（{base.as_posix()} 下没有历史报告）")
            return ExitCode.ENV_ERROR
        run_dir = max(dirs, key=lambda d: (d / "report.md").stat().st_mtime)
    else:
        run_dir = base / args.run
        if not (run_dir / "report.md").is_file():
            candidates = [d for d in base.glob(f"*{args.run}*") if (d / "report.md").is_file()] \
                if base.is_dir() else []
            if len(candidates) == 1:
                run_dir = candidates[0]
            else:
                _print(f"找不到报告：{run_dir.as_posix()}"
                       + (f"（模糊匹配到 {len(candidates)} 个，请给完整 run_id）" if candidates else ""))
                return ExitCode.ENV_ERROR

    md = run_dir / "report.md"
    text = md.read_text(encoding="utf-8", errors="replace")
    _print(f"报告路径：{md.as_posix()}")
    _print(f"JSON 快照：{(run_dir / 'report.json').as_posix()}")
    _print("-" * 72)
    lines = text.splitlines()
    shown = lines if args.lines <= 0 else lines[: args.lines]
    _print("\n".join(shown))
    if len(shown) < len(lines):
        _print(f"...（共 {len(lines)} 行，已截断；用 --lines 0 看全文）")
    if args.open_file:
        try:
            if os.name == "nt":
                os.startfile(str(md))            # type: ignore[attr-defined]
            else:
                import webbrowser
                webbrowser.open(md.as_uri())
        except OSError as exc:
            _print(f"⚠ 打开失败：{exc}")
    return ExitCode.PASS


# --------------------------------------------------------------------------- #
# 子命令：exception
# --------------------------------------------------------------------------- #
def cmd_exception_request(args: argparse.Namespace) -> int:
    root = resolve_root(args)
    spec = load_spec(root, args)
    facts = collect_git(root, args.base or "")
    requester = args.by or facts.author_name or os.environ.get("USERNAME") or "unknown"

    scope = _split_list(args.scope)
    if not scope:
        # 默认取"当前改动里命中受保护路径的文件"。
        # 为什么必须默认：rule_guard 要求 waiver 的 scope 命中受保护文件才认账，
        # 而不带 scope 的申请即使被批准也永远不生效（实测：先 block 再说"没有任何 waiver 覆盖"）。
        # 注意 --base：CI 与区间审查看的是提交区间，不是工作区；不传 --base 时
        # 工作区可能已经没有改动（都已提交），scope 会算成空 -> 例外无效。
        scope = [p for p in facts.changed if match_any(p, spec.protected_paths)]

    w = waivers.request(
        root,
        rule=args.rule.upper(),
        task=args.task or "",
        reason=args.reason,
        requested_by=requester,
        ttl_days=args.ttl_days,
        spec=spec,
        scope_files=scope,
    )
    _print(f"{STATUS_ICON[Status.WAIVED]} 已申请例外：{waivers.describe(w)}")
    _print(f"  理由：{w.reason}")
    _print(f"  范围：{scope if scope else '（未命中任何受保护文件）'}")
    if not scope:
        _print("  ⚠ 本次改动没有命中受保护路径：这条例外不会生效（无需申请，或需显式 --scope）")
    # 提前把"本地批不了"的情况说清楚，避免用户照抄命令后卡住（实测过这个坑）
    if requester in spec.rule_approvers and len(spec.rule_approvers) == 1:
        _print(f"  ⚠ 申请人 {requester} 同时是唯一审批人（rule_approvers={spec.rule_approvers}）："
               "本地无法自行批准（规则要求批准人 ≠ 申请人、且 ≠ 变更作者）")
        _print("    可选：① 用另一个审批人身份；② 由 GitHub 分支保护 + CODEOWNERS 在 PR 上批准；"
               "③ 让代理以独立身份提交（作者=agent），你再用本身份批准")
    _print(f"  批准命令（批准人必须 ≠ 申请人，且 ≠ 变更作者）："
           f"python scripts/gate/cli.py exception approve --id {w.waiver_id} --by <approver>")
    _print("  注意：未批准前本例外不生效，规则仍按原样阻断。")
    return ExitCode.PASS


def cmd_exception_approve(args: argparse.Namespace) -> int:
    root = resolve_root(args)
    spec = load_spec(root, args)
    w = waivers.approve(root, args.id, args.by, spec)
    _print(f"{STATUS_ICON[Status.WAIVED]} 已批准例外：{waivers.describe(w)}")
    return ExitCode.PASS


def cmd_exception_reject(args: argparse.Namespace) -> int:
    root = resolve_root(args)
    spec = load_spec(root, args)
    w = waivers.reject(root, args.id, args.by, args.reason, spec)
    _print(f"✗ 已驳回例外：{waivers.describe(w)}")
    _print(f"  驳回理由：{w.reject_reason}")
    return ExitCode.PASS


def cmd_exception_list(args: argparse.Namespace) -> int:
    root = resolve_root(args)
    spec = load_spec(root, args)
    items = waivers.pending(root, spec) if args.pending else waivers.list_all(root, spec)
    if not items:
        _print("（无）" if not args.pending else "（没有待批准的例外）")
        return ExitCode.PASS
    _print(f"共 {len(items)} 条例外（账本：{waivers.waiver_path(root, spec).as_posix()}）")
    for w in items:
        expire = "（已过期）" if waivers.is_expired(w, spec) else ""
        _print(f"  {waivers.describe(w)}{expire}")
        _print(f"      理由：{w.reason}  范围：{', '.join(w.scope_files) or '（不限）'}")
    return ExitCode.PASS


# --------------------------------------------------------------------------- #
# 子命令：reset
# --------------------------------------------------------------------------- #
def cmd_reset(args: argparse.Namespace) -> int:
    root = resolve_root(args)
    spec = load_spec(root, args)
    path = runner.task_path_for(root, spec, args.task)
    if not path.is_file():
        raise TaskError(f"任务契约不存在: {path.as_posix()}（reset 只作用于已声明的任务）")
    task = TaskContract.load(path)

    actor = args.by or collect_git(root).author_name or os.environ.get("USERNAME") or "human"
    if actor == task.owner:
        raise TaskError(
            f"reset 是 human-only 操作：操作者 {actor} 与任务 owner 相同，"
            "必须由另一个人执行（防止 agent 自己解开重试上限）"
        )
    if not args.reason.strip():
        raise TaskError("reset 必须给出理由（--reason）")

    before = task.attempts
    task.attempts = 0
    task.state = "DECLARED"
    task.notes.append(f"{now_iso()} reset by {actor}: {args.reason}")
    task.save(path)

    facts = collect_git(root)
    ledger.append_event(
        root,
        spec,
        event="reset",
        task=task.id,
        actor=actor,
        reason=args.reason,
        commit=facts.commit,
        attempts_before=before,
        attempts_after=0,
    )
    _print(f"{STATUS_ICON[Status.PASS]} 已重置任务 {task.id} 的尝试计数：{before} -> 0（state=DECLARED）")
    _print(f"  操作者：{actor}  理由：{args.reason}")
    _print(f"  已写入账本事件：{ledger.ledger_path(root, spec).as_posix()}")
    return ExitCode.PASS


# --------------------------------------------------------------------------- #
# 子命令：selftest
# --------------------------------------------------------------------------- #
def cmd_selftest(args: argparse.Namespace) -> int:
    root = resolve_root(args)
    try:
        from scripts.gate import selftest as st
    except ImportError as exc:
        _print(f"{STATUS_ICON[Status.ERROR]} 未找到 scripts/gate/selftest.py：{exc}")
        _print("  该文件由 checks 侧提供（负向 fixture 自检），缺失时门禁无法自证'该红时必须红'。")
        return ExitCode.SPEC_ERROR
    fn = getattr(st, "main", None)
    if not callable(fn):
        _print(f"{STATUS_ICON[Status.ERROR]} scripts/gate/selftest.py 没有可调用的 main()")
        return ExitCode.SPEC_ERROR
    extra = list(getattr(args, "extra", []) or [])
    _print(f"运行 checks 自检（仓库根：{root.as_posix()}）…")
    try:
        params = inspect.signature(fn).parameters
    except (TypeError, ValueError):
        params = {"argv": None}  # type: ignore[dict-item]
    try:
        result = fn(extra) if params else fn()
    except SystemExit as exc:                      # selftest 自己的 argparse 退出
        return int(exc.code or 0)
    return int(result or 0)


# --------------------------------------------------------------------------- #
# 解析器
# --------------------------------------------------------------------------- #
def build_parser() -> argparse.ArgumentParser:
    common = _common_parent()
    parser = argparse.ArgumentParser(
        prog="gate",
        parents=[common],
        description="CI 门禁引擎（纯标准库；spec 为唯一真相源）",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=(
            "示例：\n"
            "  python scripts/gate/cli.py declare --task T-001 --scope \"src/**,include/**\"\n"
            "  python scripts/gate/cli.py run --task T-001 --serial\n"
            "  python scripts/gate/cli.py explain SCOPE-002\n"
            "  python scripts/gate/cli.py exception list --pending\n"
        ),
    )
    sub = parser.add_subparsers(dest="command", metavar="<命令>")

    p_run = sub.add_parser("run", parents=[common], help="跑一次门禁并落盘报告/账本")
    p_run.add_argument("--update-baseline", action="store_true",
                       help="用本次实测值重算 gate/baseline.json 的棘轮基线（受保护文件，改动需 CI-001 审批）")
    p_run.add_argument("--task", default="auto",
                       help="任务契约 ID；auto（默认）= 取 gate/tasks/ 里最近更新的一份")
    p_run.add_argument("--base", default=None, metavar="A..B", help="CI 模式：只判定 A..B 区间")
    p_run.add_argument("--serial", action="store_true", help="关闭并发（规则串行执行）")
    p_run.add_argument("--skip-heavy", action="store_true", help="跳过需要编译的重规则（标 warn）")
    p_run.add_argument("--verbose", action="store_true", help="输出逐规则耗时等细节")
    p_run.add_argument("--json", action="store_true", dest="as_json", help="只打印机器可读 JSON")
    p_run.add_argument("--report-dir", default=None, help="产物目录（默认取 spec.meta.report_dir）")
    p_run.add_argument("--no-ledger", action="store_true", help="不写审计账本（默认必写）")
    p_run.set_defaults(func=cmd_run)

    p_dec = sub.add_parser("declare", parents=[common], help="声明/更新任务契约（已有则合并）")
    p_dec.add_argument("--task", required=True, help="任务 ID")
    p_dec.add_argument("--title", default=None)
    p_dec.add_argument("--phase", default=None)
    p_dec.add_argument("--owner", default=None)
    p_dec.add_argument("--scope", default=None, metavar="GLOBS", help='允许改动范围，逗号分隔，如 "src/**,include/**"')
    p_dec.add_argument("--deny", default=None, metavar="GLOBS", help="禁止触碰范围，逗号分隔")
    p_dec.add_argument("--budget", type=int, default=None, help="max_changed_lines")
    p_dec.add_argument("--max-files", type=int, default=None, help="max_changed_files")
    p_dec.add_argument("--accept", default=None, metavar="ITEMS", help='验收标准，分号分隔，如 "a;b"')
    p_dec.add_argument("--audit-tag", default=None, help="审计标签，如 audit-04")
    p_dec.set_defaults(func=cmd_declare)

    p_st = sub.add_parser("status", parents=[common], help="当前契约 + 最近账本 + 待批准例外")
    p_st.add_argument("--task", default=None, help="指定任务（默认取最近更新的契约）")
    p_st.set_defaults(func=cmd_status)

    p_ex = sub.add_parser("explain", parents=[common], help="解释某条规则的阈值与修复/例外路径")
    p_ex.add_argument("rule_id", metavar="RULE_ID")
    p_ex.set_defaults(func=cmd_explain)

    p_rep = sub.add_parser("report", parents=[common], help="查看历史报告")
    p_rep.add_argument("--run", default="last", help="last 或具体 run_id（支持子串匹配）")
    p_rep.add_argument("--lines", type=int, default=120, help="打印行数，0=全文")
    p_rep.add_argument("--open", action="store_true", dest="open_file", help="用系统默认程序打开报告")
    p_rep.set_defaults(func=cmd_report)

    p_wa = sub.add_parser("exception", parents=[common], help="例外（waiver）流程")
    wa_sub = p_wa.add_subparsers(dest="exception_command", metavar="<动作>")

    p_wr = wa_sub.add_parser("request", parents=[common], help="申请例外（待批准）")
    p_wr.add_argument("--rule", required=True, help="规则 ID，如 DOC-001")
    p_wr.add_argument("--reason", required=True, help="为什么必须越轨")
    p_wr.add_argument("--task", default=None, help="关联任务 ID")
    p_wr.add_argument("--base", default="",
                      help="按提交区间计算默认范围（如 origin/main..HEAD）；不传则按工作区改动")
    p_wr.add_argument("--ttl-days", type=int, default=None, help="有效天数（默认取规则/CI-001 的 waiver_max_age_days）")
    p_wr.add_argument("--by", default=None, help="申请人（默认取 git user.name）")
    p_wr.add_argument("--scope", default=None, metavar="GLOBS", help="仅对匹配文件生效，逗号分隔")
    p_wr.set_defaults(func=cmd_exception_request)

    p_wap = wa_sub.add_parser("approve", parents=[common], help="批准例外（须非申请人且属 rule_approvers）")
    p_wap.add_argument("--id", required=True, help="waiver_id，如 W-260308-ab12")
    p_wap.add_argument("--by", required=True, help="批准人（必须在 spec.meta.rule_approvers 内）")
    p_wap.set_defaults(func=cmd_exception_approve)

    p_wj = wa_sub.add_parser("reject", parents=[common], help="驳回/撤销例外")
    p_wj.add_argument("--id", required=True)
    p_wj.add_argument("--by", required=True)
    p_wj.add_argument("--reason", required=True)
    p_wj.set_defaults(func=cmd_exception_reject)

    p_wl = wa_sub.add_parser("list", parents=[common], help="列出例外")
    p_wl.add_argument("--pending", action="store_true", help="只看待批准")
    p_wl.set_defaults(func=cmd_exception_list)

    p_rs = sub.add_parser("reset", parents=[common], help="人工解锁重试上限（human-only）")
    p_rs.add_argument("--task", required=True)
    p_rs.add_argument("--reason", required=True)
    p_rs.add_argument("--by", default=None, help="操作者（默认取 git user.name；必须 ≠ 任务 owner）")
    p_rs.set_defaults(func=cmd_reset)

    p_self = sub.add_parser("selftest", parents=[common], help="运行 checks 侧的负向自检")
    # selftest 的参数（--list/--only/--verbose…）不在这里声明：
    # main() 用 parse_known_args 原样转发未知参数，避免与上游 argparse 冲突。
    # 注意：--root/--spec 请写在子命令之前（如 `gate --root X selftest --list`）。
    p_self.set_defaults(func=cmd_selftest)

    return parser


# --------------------------------------------------------------------------- #
# 入口
# --------------------------------------------------------------------------- #
def main(argv: list[str] | None = None) -> int:
    """返回 ExitCode；任何异常都转成可读信息 + 对应退出码，不向外抛。"""
    _setup_stdout()
    parser = build_parser()
    # parse_known_args：selftest 需要把 --list/--only 等参数原样转发给上游 argparse；
    # 其它子命令一旦出现未知参数仍按 argparse 惯例报错（不静默吞掉拼错的选项）。
    args, extras = parser.parse_known_args(argv)
    if extras and getattr(args, "command", None) != "selftest":
        parser.error(f"unrecognized arguments: {' '.join(extras)}")
    if getattr(args, "command", None) == "selftest":
        setattr(args, "extra", list(extras))

    if not getattr(args, "command", None):
        parser.print_help()
        return ExitCode.PASS

    if args.command == "exception" and not getattr(args, "exception_command", None):
        _print("用法：gate exception {request|approve|reject|list} ...")
        _print('例如：gate exception request --rule DOC-001 --reason "为什么这次必须越轨"')
        _print('      gate exception approve --id W-260308-ab12 --by xzgjj')
        _print("      gate exception list --pending")
        return ExitCode.PASS

    handler = getattr(args, "func", None)
    if handler is None:                            # 理论上不可达
        parser.print_help()
        return ExitCode.PASS

    try:
        return int(handler(args) or 0)
    except SpecError as exc:
        _print(f"{STATUS_ICON[Status.ERROR]} 规格错误（exit 3）：{exc}")
        return ExitCode.SPEC_ERROR
    except runner.EnvError as exc:
        _print(f"{STATUS_ICON[Status.ERROR]} 环境错误（exit 4）：{exc}")
        return ExitCode.ENV_ERROR
    except (TaskError, WaiverError) as exc:
        _print(f"{STATUS_ICON[Status.ERROR]} 操作被拒绝（exit 4）：{exc}")
        return ExitCode.ENV_ERROR
    except KeyboardInterrupt:
        _print(f"\n{STATUS_ICON[Status.HALTED]} 已中断：没有产生判定结果（本次运行作废）")
        return ExitCode.ENV_ERROR
    except OSError as exc:
        _print(f"{STATUS_ICON[Status.ERROR]} IO 错误（exit 4）：{exc}")
        return ExitCode.ENV_ERROR
    except Exception as exc:                       # 最后一道防线：绝不静默
        _print(f"{STATUS_ICON[Status.ERROR]} 未预期错误（exit 4）：{type(exc).__name__}: {exc}")
        traceback.print_exc()
        return ExitCode.ENV_ERROR


if __name__ == "__main__":
    raise SystemExit(main())
