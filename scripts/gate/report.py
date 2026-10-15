"""报告渲染：一次运行同时产出"给人看的"与"给机器读的"两种形态。

为什么存在：门禁失败时最大的浪费是"看不出来该改什么"。报告强制把机器判定
（规则/观察值/阈值/hint）与人的判断（例外、转人工）分区，人照着做、机器照着解析。
产物：local/gate/<run_id>/ 下的 report.json、report.md、evidence/index.json。
"""
from __future__ import annotations

import json
import os
import sys
import time
from pathlib import Path
from typing import Any

from .model import STATUS_ICON, GateReport, RuleResult, Status

#: GateReport.environment 里由 runner 写入、本模块消费的键（集中定义避免拼错）
ENV_NOTES = "notes"
ENV_ERRORS = "errors"
ENV_WAIVERS = "applied_waivers"
ENV_HALT = "halt"
ENV_SKIPPED = "skipped_rules"

_ANSI = {
    "reset": "\033[0m",
    "bold": "\033[1m",
    "dim": "\033[2m",
    "red": "\033[31m",
    "green": "\033[32m",
    "yellow": "\033[33m",
    "magenta": "\033[35m",
    "cyan": "\033[36m",
}

_VERDICT_COLOR = {
    Status.PASS: "green",
    Status.WARN: "yellow",
    Status.BLOCK: "red",
    Status.MANUAL: "magenta",
    Status.HALTED: "red",
    Status.ERROR: "red",
    Status.WAIVED: "cyan",
}


# --------------------------------------------------------------------------- #
# 目录与写入
# --------------------------------------------------------------------------- #
def run_dir_for(root: str | Path, report: GateReport, spec: Any = None) -> Path:
    """本次运行的产物目录；report.report_dir 优先（保留调用方指定）。"""
    base = report.report_dir or (spec.meta.get("report_dir", "local/gate") if spec else "local/gate")
    d = Path(base)
    if not d.is_absolute():
        d = Path(root) / d
    if d.name != report.run_id:          # 避免重复追加 run_id
        d = d / report.run_id
    return d


def write_reports(root: str | Path, report: GateReport, spec: Any = None) -> tuple[Path, Path]:
    """写 report.json / report.md / evidence/index.json，返回 (json, md) 路径。"""
    root_p = Path(root)
    d = run_dir_for(root_p, report, spec)
    d.mkdir(parents=True, exist_ok=True)

    # 先把最终目录写回报告（JSON 里要能看到产物位置），再序列化。
    try:
        rel = d.resolve().relative_to(root_p.resolve()).as_posix()
    except ValueError:
        rel = d.as_posix()
    report.report_dir = rel

    ev = d / "evidence"
    ev.mkdir(parents=True, exist_ok=True)
    json_path = d / "report.json"
    md_path = d / "report.md"

    _atomic_text(json_path, report.to_json() + "\n")
    _atomic_text(md_path, render_markdown(report, spec))
    _atomic_text(ev / "index.json", json.dumps(_evidence_index(root_p, report), ensure_ascii=False, indent=2) + "\n")
    return json_path, md_path


def _atomic_text(path: Path, text: str) -> None:
    tmp = path.with_name(f"{path.name}.tmp{os.getpid()}")
    with open(tmp, "w", encoding="utf-8", newline="\n") as fh:
        fh.write(text)
        fh.flush()
        os.fsync(fh.fileno())
    os.replace(tmp, path)


def _evidence_index(root: Path, report: GateReport) -> dict[str, Any]:
    """证据索引：只原样记录 finding.evidence 指向的路径，不复制大文件。"""
    items: list[dict[str, Any]] = []
    for r in report.results:
        for f in r.findings:
            if not f.evidence:
                continue
            ev = str(f.evidence).replace("\\", "/")
            items.append(
                {
                    "rule": r.rule_id,
                    "subject": f.subject,
                    "evidence": ev,
                    "line": f.line,
                    "status": f.status.value,
                    "exists": (root / ev).exists(),
                }
            )
    return {
        "run_id": report.run_id,
        "generated_at": time.strftime("%Y-%m-%dT%H:%M:%S%z"),
        "note": "本目录只索引证据路径，不复制原始文件；路径相对仓库根。",
        "items": items,
    }


# --------------------------------------------------------------------------- #
# 控制台摘要
# --------------------------------------------------------------------------- #
def _use_color() -> bool:
    if os.environ.get("NO_COLOR"):
        return False
    try:
        if not sys.stdout.isatty():
            return False
    except (AttributeError, ValueError):
        return False
    if os.name == "nt":                  # 老 conhost 需要显式打开 VT 处理
        try:
            import ctypes

            k = ctypes.windll.kernel32  # type: ignore[attr-defined]
            mode = ctypes.c_uint32()
            handle = k.GetStdHandle(-11)
            if not k.GetConsoleMode(handle, ctypes.byref(mode)):
                return False
            return bool(k.SetConsoleMode(handle, mode.value | 0x0004))
        except Exception:
            return False
    return True


def render_console(report: GateReport, color: bool | None = None) -> str:
    """终端摘要（纯文本；有 ANSI 条件时着色）。不依赖任何第三方库。"""
    use = _use_color() if color is None else color

    def c(text: str, key: str) -> str:
        return f"{_ANSI[key]}{text}{_ANSI['reset']}" if use and key in _ANSI else text

    icon = STATUS_ICON.get(report.verdict, "?")
    lines: list[str] = []
    lines.append(
        c(f"{icon} 门禁 {report.verdict.value.upper()}", _VERDICT_COLOR.get(report.verdict, "bold"))
        + f"  run={report.run_id}"
    )
    lines.append(
        f"   task={report.task or 'adhoc'}  attempt={report.attempt}  commit={report.commit or '-'}"
        f"  branch={report.branch or '-'}  用时 {report.duration_s:.2f}s"
    )
    counts = report.counts()
    order = [Status.PASS, Status.WARN, Status.WAIVED, Status.MANUAL, Status.BLOCK, Status.HALTED, Status.ERROR]
    parts = [f"{STATUS_ICON[s]} {s.value}={counts[s.value]}" for s in order if counts.get(s.value)]
    lines.append("   规则统计：" + ("  ".join(parts) if parts else "（无规则执行）"))

    for r in report.results:
        if r.status in (Status.BLOCK, Status.MANUAL, Status.ERROR, Status.HALTED):
            for f in r.findings:
                if f.status.is_failing:
                    lines.append(c(f"   {STATUS_ICON[f.status]} {r.rule_id} {f.subject}: {f.message}", "red"))
            if r.error:
                lines.append(c(f"   {STATUS_ICON[Status.ERROR]} {r.rule_id} 检查出错: {r.error}", "red"))
        elif r.status is Status.WAIVED:
            lines.append(
                c(f"   ⊘ {r.rule_id} 已有例外（{r.waiver_id} by {r.waived_by}）", "cyan")
            )
        elif r.status is Status.WARN:
            for f in r.findings:
                if f.status is Status.WARN:
                    lines.append(c(f"   ⚠ {r.rule_id} {f.subject}: {f.message}", "yellow"))

    env = report.environment or {}
    for err in env.get(ENV_ERRORS, []) or []:
        lines.append(c(f"   ! {err}", "red"))
    for note in env.get(ENV_NOTES, []) or []:
        lines.append(c(f"   · {note}", "dim"))
    halt = env.get(ENV_HALT)
    if halt:
        lines.append(c(f"   ⛔ {halt.get('message', '')}", "red"))
        lines.append(c(f"   → {halt.get('hint', '')}", "yellow"))
    if report.report_dir:
        lines.append(f"   报告：{report.report_dir}/report.md")
    return "\n".join(lines)


# --------------------------------------------------------------------------- #
# Markdown 报告
# --------------------------------------------------------------------------- #
def _esc(value: Any) -> str:
    return str(value).replace("|", "\\|").replace("\n", " ").strip()


def _code(value: Any) -> str:
    return f"`{_esc(value)}`" if value not in (None, "") else "`-`"


def _one_liner(r: RuleResult) -> str:
    if r.error:
        return f"检查出错：{_esc(r.error)}"
    real = [f for f in r.findings if f.status is not Status.PASS]
    if not real:
        return "通过"
    first = real[0].message or real[0].subject
    text = _esc(first)
    return text if len(text) <= 80 else text[:77] + "…"


def _failing(results: list[RuleResult], statuses: tuple[Status, ...]) -> list[tuple[RuleResult, Any]]:
    out: list[tuple[RuleResult, Any]] = []
    for r in results:
        for f in r.findings:
            if f.status in statuses:
                out.append((r, f))
    return out


def _render_finding(rule: RuleResult, f: Any) -> list[str]:
    lines = [
        f"#### {STATUS_ICON.get(f.status, '?')} {rule.rule_id} · {_code(f.subject)}",
        "",
        f"- 规则：{rule.rule_id} {rule.title}（mode={rule.mode}，状态={f.status.value}）",
        f"- 观察值 observed：{_code(f.observed)}",
        f"- 阈值 threshold：{_code(f.threshold)}",
        f"- 说明：{f.message}",
        f"- **修复提示（hint，原样输出）：** {f.hint if f.hint else '（该检查未提供 hint）'}",
    ]
    if f.evidence:
        loc = f"{f.evidence}:{f.line}" if f.line else str(f.evidence)
        lines.append(f"- 证据：{_code(loc)}")
    lines.append("")
    return lines


def render_markdown(report: GateReport, spec: Any = None) -> str:
    env = report.environment or {}
    L: list[str] = []
    verdict_icon = STATUS_ICON.get(report.verdict, "?")

    # ---- 1. 头部 ---------------------------------------------------------- #
    L.append(f"# {verdict_icon} 门禁报告 · {report.task or 'adhoc'} · {report.verdict.value.upper()}")
    L.append("")
    L.append("| 项 | 值 |")
    L.append("| --- | --- |")
    spec_ver = spec.meta.get("spec_version", "-") if spec else "-"
    L.append(f"| run_id | {_code(report.run_id)} |")
    L.append(f"| task | {_code(report.task or 'adhoc')} |")
    L.append(f"| attempt | {_code(report.attempt)} |")
    L.append(f"| commit | {_code(report.commit or '-')} |")
    L.append(f"| branch | {_code(report.branch or '-')} |")
    L.append(f"| verdict / exit_code | {verdict_icon} {report.verdict.value} / {report.exit_code} |")
    L.append(f"| 耗时 | {report.duration_s:.2f}s |")
    L.append(f"| spec_hash | {_code(report.spec_hash)}（spec_version={spec_ver}） |")
    L.append(
        f"| 环境 | Python {_esc(env.get('python', '-'))} · {_esc(env.get('platform', '-'))} · "
        f"git={_esc((env.get('git') or {}).get('mode', '-'))} |")
    L.append(
        f"| 改动 | {_esc((env.get('git') or {}).get('changed_files', '-'))} 个文件 / "
        f"{_esc((env.get('git') or {}).get('lines_changed', '-'))} 行 |")
    L.append(f"| 产物目录 | {_code(report.report_dir or '-')} |")
    L.append("")

    halt = env.get(ENV_HALT)
    if halt:
        L.append(f"> ⛔ **本次运行已 HALTED（退出码 2）：** {halt.get('message', '')}")
        L.append(f"> ")
        L.append(f"> {halt.get('semantics', '')}")
        L.append(f"> ")
        L.append(f"> 下一步：`{halt.get('hint', '')}`")
        L.append("")

    # ---- 2. 汇总表（机器判定） -------------------------------------------- #
    L.append("## 1. 规则汇总（机器判定）")
    L.append("")
    L.append("| 规则 | 状态 | 一句话结论 | 耗时 |")
    L.append("| --- | --- | --- | --- |")
    for r in report.results:
        icon = STATUS_ICON.get(r.status, "?")
        L.append(f"| {r.rule_id} {_esc(r.title)} | {icon} {r.status.value} | {_one_liner(r)} | {r.duration_ms}ms |")
    if not report.results:
        L.append("| （无） | - | 未执行任何规则 | - |")
    L.append("")

    # ---- 3. 阻断项 -------------------------------------------------------- #
    blocked = _failing(report.results, (Status.BLOCK, Status.ERROR, Status.HALTED))
    L.append("## 2. ❌ 阻断项（必须处理，机器判定）")
    L.append("")
    if blocked:
        for rule, f in blocked:
            L.extend(_render_finding(rule, f))
        for r in report.results:
            if r.error:
                L.append(f"#### 💥 {r.rule_id} · 检查自身出错")
                L.append("")
                L.append(f"- 错误：{_esc(r.error)}")
                L.append("- 说明：这是门禁脚本/环境问题，不是代码问题；修好后重跑，不要用例外掩盖。")
                L.append("")
    else:
        L.append("_（无）_")
        L.append("")

    # ---- 4. 例外项（人的判断） -------------------------------------------- #
    applied = env.get(ENV_WAIVERS) or []
    L.append("## 3. ⊘ 例外项（人的判断：已批准的越轨）")
    L.append("")
    if applied:
        L.append("| waiver | 规则 | 申请人 | 批准人 | 理由 | 到期 | 范围 |")
        L.append("| --- | --- | --- | --- | --- | --- | --- |")
        for w in applied:
            scope = ", ".join(w.get("scope_files") or []) or "（不限）"
            L.append(
                f"| {_code(w.get('waiver_id'))} | {_code(w.get('rule'))} | {_esc(w.get('requested_by'))} | "
                f"{_esc(w.get('approved_by'))} | {_esc(w.get('reason'))} | {_esc(w.get('expires_at'))} | {_esc(scope)} |"
            )
        L.append("")
        L.append("> 例外是【人的判断】，不代表问题消失；到期后本条会重新变成阻断项。")
        # 逐条列出"这条例外实际豁免了哪些 finding"。
        # 为什么必须列：只看上面那张表，读者无法判断"被放过的到底是什么"——
        # 实测过一条 scope=[src/a.cpp] 的例外面上看没问题，实际顺带豁免了 LICENSE 的越界。
        L.append("")
        L.append("**被本次例外豁免的具体条目**（复核时请看这里，而不是只看上表）")
        L.append("")
        waived_rows: list[str] = []
        for rule in report.results:
            for f in rule.findings:
                if f.status is Status.WAIVED:
                    waived_rows.append(
                        f"- **{rule.rule_id}** {_code(f.subject)}：{_esc(f.message)} "
                        f"（observed={_esc(f.observed)} / threshold={_esc(f.threshold)}）"
                    )
        if waived_rows:
            L.extend(waived_rows)
        else:
            L.append("_（本轮的例外没有命中任何 finding，可能范围写错了）_")
    else:
        L.append("_（无）_")
    L.append("")

    # ---- 5. 警告项 -------------------------------------------------------- #
    warns = _failing(report.results, (Status.WARN,))
    L.append("## 4. ⚠ 警告项（不阻断，但已记账）")
    L.append("")
    if warns:
        for rule, f in warns:
            extra = f"（observed={_esc(f.observed)} / threshold={_esc(f.threshold)}）"
            L.append(f"- **{rule.rule_id}** {_code(f.subject)}：{f.message} {extra}")
        L.append("")
    else:
        L.append("_（无）_")
        L.append("")

    # ---- 6. 转人工 -------------------------------------------------------- #
    manual = _failing(report.results, (Status.MANUAL,))
    L.append("## 5. ⏸ 转人工判断项（机器拒绝自动下结论）")
    L.append("")
    L.append("> 需要人类判断：**是否值得保留 / 是否必要 / 行为变更是否合理**。"
             "机器只负责把事实摆齐，批准与否由人负责。")
    L.append("")
    if manual:
        for rule, f in manual:
            L.extend(_render_finding(rule, f))
            L.append("- 人的判断点：这个改动是否必要？是否可以更小？行为变更是否被测试覆盖？")
            L.append("")
    else:
        L.append("_（无）_")
        L.append("")

    # ---- 7. 证据与下一步 -------------------------------------------------- #
    L.append("## 6. 证据索引与下一步命令")
    L.append("")
    ev_dir = f"{report.report_dir}/evidence/index.json" if report.report_dir else "evidence/index.json"
    ev_count = sum(1 for r in report.results for f in r.findings if f.evidence)
    L.append(f"- 证据索引：`{ev_dir}`（共 {ev_count} 条，只记路径不复制文件）")
    L.append(f"- 机器可读快照：`{report.report_dir}/report.json`")
    L.append("")
    L.append("```bash")
    L.append(f"python scripts/gate/cli.py report --run {report.run_id}     # 重看本次报告")
    for rule, _f in blocked[:3]:
        L.append(f"python scripts/gate/cli.py explain {rule.rule_id}            # 该规则怎么修/怎么申请例外")
    if blocked:
        rid = blocked[0][0].rule_id
        L.append(f'python scripts/gate/cli.py exception request --rule {rid} --reason "为什么这次必须越轨"')
    if halt:
        L.append(f'python scripts/gate/cli.py reset --task {report.task or "TASK-ID"} --reason "为什么允许再试"')
    L.append("```")
    L.append("")

    skipped = env.get(ENV_SKIPPED) or []
    if skipped:
        L.append(f"> ⚠ 本次跳过了重规则（--skip-heavy）：{', '.join(_esc(s) for s in skipped)}。"
                 "跳过不等于通过，请在完整 CI 中补跑。")
        L.append("")

    notes = env.get(ENV_NOTES) or []
    errors = env.get(ENV_ERRORS) or []
    if notes or errors:
        L.append("## 附：引擎备注")
        L.append("")
        for e in errors:
            L.append(f"- ❗ {_esc(e)}")
        for n in notes:
            L.append(f"- {_esc(n)}")
        L.append("")
    return "\n".join(L)
