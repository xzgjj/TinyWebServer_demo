"""C 族：改动纪律（SCOPE-001 范围白名单 / SCOPE-002 Diff 预算）。

SCOPE-001 是"无人认领的改动一律阻断"的落地：没有任务契约时直接 BLOCK。
SCOPE-002 是唯一被授权写盘的非基线 check（写自己的 evidence 文件）。
"""
from __future__ import annotations

from ..context import Context, match_any
from ..model import Finding, Status
from ..spec import Rule
from . import evidence_path, write_text_atomic

CHECKS = ("scope", "diff_budget")


def _git_unavailable(rule_id: str, rule: Rule) -> Finding:
    """git 读不到事实时的统一处置。

    为什么必须是 ERROR 而不是 PASS：改动集为空时，SCOPE/预算类规则会"零对象"通过 ——
    那是**看起来通过，其实什么都没检查**（实测：MSYS2 环境 PATH 里没有 git，
    门禁直接判 PASS）。环境不满足必须显式失败，绝不能静默放行。
    """
    return Finding(
        rule=rule_id, status=Status.ERROR, subject="(git 事实)",
        message="git 不可用（未找到 git 或不在仓库工作区），无法获取改动集：范围与预算类判定不可信",
        observed="git rev-parse 失败", threshold="可用的 git 工作区",
        level="error",
        hint="安装 git 并确保在仓库内运行；或改用 --base 区间模式在 CI 中执行。",
    )


# --------------------------------------------------------------------------- #
# SCOPE-001 改动范围白名单
# --------------------------------------------------------------------------- #
def scope(ctx: Context, rule: Rule) -> list[Finding]:
    """改动的每个文件都必须落在任务契约允许范围内，且不得触碰 deny / default_deny。

    判定顺序（先契约后默认，先 deny 后 allow）：
      1. 无契约 且 require_task_contract -> BLOCK（"未声明任务契约"）；
      2. 命中 deny 或 default_deny -> BLOCK（硬阻断，level=error）；
      3. task.allow 非空时，逐个文件必须命中 allow，否则 BLOCK；
         allow 为空 = "未限定范围"，此时只受 deny / default_deny 约束。
    """
    findings: list[Finding] = []
    if not ctx.git.available:
        return [_git_unavailable(rule.id, rule)]
    require_contract = bool(rule.get("require_task_contract", False))
    default_deny: list[str] = list(rule.get("default_deny") or [])
    task = ctx.task

    # 范围判定用 git.changed（含被删除的文件）；affected() 会过滤掉"已不存在"的路径，
    # 用它会把"删除 LICENSE"这类改动整个漏掉（实测 exit 0）。
    changed = list(ctx.git.changed)
    if task is None:
        if require_contract:
            return [
                Finding(
                    rule=rule.id, status=Status.BLOCK, subject="(任务契约)",
                    message="未声明任务契约：无契约的改动不允许进入门禁",
                    observed={"task": None, "changed": len(changed)},
                    threshold="存在 gate/tasks/<id>.json 且处于活动状态",
                    level="error",
                    hint="先声明任务契约（含 allow/deny 与 diff 预算），再提交改动。",
                )
            ]
        findings.append(
            Finding(
                rule=rule.id, status=Status.WARN, subject="(任务契约)",
                message="未声明任务契约，仅按 params.default_deny 判定（require_task_contract=false）",
                observed=None, threshold="建议声明契约",
                hint="声明契约可获得 allow 白名单与 diff 预算约束。",
            )
        )

    deny = list(task.deny) if task else []
    allow = list(task.allow) if task else []
    # 两类判定对象刻意不同：
    #   * allow（"你被允许改什么"）只看参与判定的文件（剔除 build/**、exclude_globs 等）；
    #   * deny/default_deny（"什么绝对不能碰"）必须看**全部**改动 —— 否则
    #     default_deny 里的 third_party/** 会被 exclude_globs 先剔除，永远命中不了，
    #     等于这条禁止形同虚设（实测：改 third_party/lib.hpp 判 pass）。
    targets = [p for p in changed if not ctx.excluded(p)]
    deny_targets = list(changed)

    if not targets and not deny_targets:
        findings.append(
            Finding(
                rule=rule.id, status=Status.PASS, subject="(改动文件)",
                message="本次改动没有任何参与判定的文件",
                observed=0, threshold=">=1", level="info",
            )
        )
        return findings

    denied: list[tuple[str, str]] = []      # (path, 命中的模式)
    outside: list[str] = []
    for path in deny_targets:
        hit = _first_match(path, deny)
        if hit is not None:
            denied.append((path, f"task.deny:{hit}"))
            continue
        hit = _first_match(path, default_deny)
        if hit is not None:
            denied.append((path, f"default_deny:{hit}"))
    for path in targets:
        if allow and not match_any(path, allow):
            outside.append(path)

    for path, why in denied:
        findings.append(
            Finding(
                rule=rule.id, status=Status.BLOCK, subject=path,
                message=f"改动落在禁止范围内（{why}）",
                observed=path, threshold=f"不得命中 {why}", level="error",
                hint="撤回该文件的改动；确有必要须走 waiver（CI-001）由非作者审批。",
            )
        )
    for path in outside:
        findings.append(
            Finding(
                rule=rule.id, status=Status.BLOCK, subject=path,
                message=f"改动不在任务契约允许范围内（task.allow={allow}）",
                observed=path, threshold=allow, level="error",
                hint="把该文件加入契约 allow（需说明理由），或撤回改动。",
            )
        )

    if not denied and not outside:
        findings.append(
            Finding(
                rule=rule.id, status=Status.PASS, subject="(改动文件)",
                message=(
                    f"{len(targets)} 个改动文件全部在范围内"
                    + (f"（{len(allow)} 条 allow 模式）" if allow else "（allow 未限定，仅受 deny 约束）")
                    + (f"；禁止清单按全部 {len(deny_targets)} 个改动核对" if deny_targets != targets else "")
                ),
                observed=len(targets), threshold=len(targets), level="info",
            )
        )
    return findings


def _first_match(path: str, patterns: list[str]) -> str | None:
    for pat in patterns:
        if match_any(path, [pat]):
            return pat
    return None


# --------------------------------------------------------------------------- #
# SCOPE-002 Diff 预算
# --------------------------------------------------------------------------- #
def diff_budget(ctx: Context, rule: Rule) -> list[Finding]:
    """改动行数/文件数超预算即 BLOCK；本 check 负责写出 diffstat 证据文件。"""
    findings: list[Finding] = []
    if not ctx.git.available:
        return [_git_unavailable(rule.id, rule)]
    min_lines = 0
    if ctx.task is not None:
        max_lines, max_files, min_lines = ctx.task.effective_limits(rule)
        source = f"task:{ctx.task.id}"
    else:
        max_lines = int(rule.get("max_changed_lines", 800))
        max_files = int(rule.get("max_changed_files", 24))
        min_lines = int(rule.get("min_changed_lines", 0))
        source = "spec 默认（无任务契约）"

    # 契约预算的**硬上限**：契约文件由代理自己写，如果允许它任意调大预算，
    # "Diff 预算"这条规则就等于不存在（自己给自己发许可证）。
    # 硬上限写在本规则里（受保护文件），代理要突破必须改规则 -> 触发 CI-001。
    cap_lines = int(rule.get("max_changed_lines_cap", 0) or 0)
    cap_files = int(rule.get("max_changed_files_cap", 0) or 0)
    if cap_lines and max_lines > cap_lines:
        findings.append(
            Finding(
                rule=rule.id, status=Status.BLOCK, subject="(任务契约预算)",
                message=(
                    f"契约声明的行数预算 {max_lines} 超过硬上限 {cap_lines}："
                    "契约不能自行放宽预算，只能收紧"
                ),
                observed=max_lines, threshold=cap_lines, level="error",
                hint=f"把 {source} 的 max_changed_lines 调到 <= {cap_lines}，或申请人工例外。",
            )
        )
        max_lines = cap_lines
    if cap_files and max_files > cap_files:
        findings.append(
            Finding(
                rule=rule.id, status=Status.BLOCK, subject="(任务契约预算)",
                message=f"契约声明的文件数预算 {max_files} 超过硬上限 {cap_files}",
                observed=max_files, threshold=cap_files, level="error",
            )
        )
        max_files = cap_files

    lines = ctx.git.lines_changed(ctx.affected())   # 只算参与判定的文件（不含 build/** 等）
    affected = ctx.affected()
    files = len(affected)
    added, deleted = ctx.git.added(), ctx.git.deleted()

    text = _render_diffstat(ctx, source, max_lines, max_files, min_lines, lines, files, added, deleted)
    evidence_rel: str | None = None
    try:
        abs_path, evidence_rel = evidence_path(ctx, "diffstat")
        write_text_atomic(abs_path, text)
    except OSError as exc:
        evidence_rel = None
        findings.append(
            Finding(
                rule=rule.id, status=Status.WARN, subject="(证据文件)",
                message=f"diffstat 证据写出失败（不影响判定，但无法复核）: {exc}",
                observed=type(exc).__name__, threshold="可写 local/gate/<run_id>/evidence/",
            )
        )

    if lines > max_lines:
        findings.append(
            Finding(
                rule=rule.id, status=Status.BLOCK, subject="(diff 行数)",
                message=f"改动 {lines} 行 > 上限 {max_lines} 行（{source}，口径 {rule.get('count_mode')}）",
                observed=lines, threshold=max_lines, level="error", evidence=evidence_rel,
                hint="缩小任务颗粒度：把这次改动拆成多个任务，或先提交与预算相符的部分。",
            )
        )
    if files > max_files:
        findings.append(
            Finding(
                rule=rule.id, status=Status.BLOCK, subject="(diff 文件数)",
                message=f"改动 {files} 个文件 > 上限 {max_files} 个（{source}）",
                observed=files, threshold=max_files, level="error", evidence=evidence_rel,
                hint="把无关文件移出本次改动；一个任务只解决一个问题。",
            )
        )
    if lines < min_lines:
        findings.append(
            Finding(
                rule=rule.id, status=Status.PASS, subject="(diff 行数)",
                message=f"改动 {lines} 行 < 契约下界 {min_lines} 行（仅记录，不阻断）",
                observed=lines, threshold=min_lines, level="info", evidence=evidence_rel,
            )
        )

    if lines <= max_lines and files <= max_files:
        findings.append(
            Finding(
                rule=rule.id, status=Status.PASS, subject="(diff 预算)",
                message=(
                    f"改动 {lines}/{max_lines} 行、{files}/{max_files} 文件"
                    f"（+{added}/-{deleted}，{source}）"
                ),
                observed=lines, threshold=max_lines, level="info", evidence=evidence_rel,
            )
        )
    return findings


def _render_diffstat(
    ctx: Context,
    source: str,
    max_lines: int,
    max_files: int,
    min_lines: int,
    lines: int,
    files: int,
    added: int,
    deleted: int,
) -> str:
    lines_out: list[str] = [
        "# SCOPE-002 diff 预算证据（由 diff_budget check 写出）",
        f"run_id       : {ctx.run_id}",
        f"task         : {ctx.task.id if ctx.task else '(无契约)'}",
        f"commit       : {ctx.git.commit}",
        f"git_mode     : {ctx.git.mode} {ctx.git.ref_range}".rstrip(),
        f"limits_from  : {source}",
        f"max_lines    : {max_lines}",
        f"max_files    : {max_files}",
        f"min_lines    : {min_lines}",
        f"lines_changed: {lines}",
        f"files_changed: {files}",
        f"added        : {added}",
        f"deleted      : {deleted}",
        "",
        "## 参与判定的文件（仓库相对路径，已剔除 exclude_globs）",
    ]
    for rel in sorted(ctx.affected()):
        a, d = ctx.git.numstat.get(rel, (0, 0))
        status = ctx.git.status_map.get(rel, "?")
        lines_out.append(f"{a:>7}\t{d:>7}\t{status:<3}\t{rel}")
    if not ctx.affected():
        lines_out.append("(无)")
    return "\n".join(lines_out) + "\n"
