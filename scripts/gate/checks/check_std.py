"""E 族（标准部分）：STD-001 C++ 标准特性白名单。

判定口径：
  * `forbidden_unless_guarded` 里的记号，出现在 **不含 guard_macro（TWS_HAS_）的
    `#if` 保护块内**（含条件编译之外的一切位置）即 BLOCK；
  * `allowed` 里的记号出现即记 PASS（证明"白名单确实在生效"，不是空跑）；
  * 条件编译的分支要按 `#if/#elif/#else/#endif` 逐层跟踪——`#else` 分支的保护
    与 `#if` 分支不同，不能一并算作"已保护"。

只读；给出 文件:行号 与可直接照做的修复指引。
"""
from __future__ import annotations

import re

from ..context import Context, strip_comments_preserve_lines
from ..model import Finding, Status
from ..spec import Rule
from . import glob_root

CHECKS = ("std_features",)

_IF_RE = re.compile(r"(?m)^[ \t]*#[ \t]*(if|ifdef|ifndef)\b(?P<cond>.*)$")
_ELIF_RE = re.compile(r"(?m)^[ \t]*#[ \t]*(elif|else)\b(?P<cond>.*)$")
_ENDIF_RE = re.compile(r"(?m)^[ \t]*#[ \t]*endif\b")


def std_features(ctx: Context, rule: Rule) -> list[Finding]:
    """扫描改动代码文件：交集外特性必须写在 guard_macro 保护块里。"""
    findings: list[Finding] = []
    forbidden: list[str] = list(rule.get("forbidden_unless_guarded") or [])
    allowed: list[str] = list(rule.get("allowed") or [])
    guard_macro = str(rule.get("guard_macro", "TWS_HAS_"))

    files = ctx.code_files()
    if not files:
        files = [
            rel
            for rel in glob_root(ctx.root, "src/**/*") + glob_root(ctx.root, "include/**/*")
            if rel.endswith(tuple(ctx.spec.code_exts))
        ][:200]
        if files:
            findings.append(
                Finding(
                    rule=rule.id, status=Status.PASS, subject="(扫描范围)",
                    message=f"本次改动没有 C++ 文件，按核心源集扫描 {len(files)} 个文件",
                    observed=len(files), threshold="改动文件优先", level="info",
                )
            )

    if not files:
        return findings + [
            Finding(
                rule=rule.id, status=Status.WARN, subject="(扫描范围)",
                message="没有可扫描的 C++ 文件（改动为空且 src/include 为空），STD-001 未实际执行",
                observed=0, threshold=">=1", level="warning",
                hint="确认仓库布局与 [meta].code_exts 配置。",
            )
        ]

    violations: list[tuple[str, int, str, str]] = []   # (file, line, token, 上下文)
    allowed_hits: list[tuple[str, int, str]] = []
    unreadable: list[str] = []

    for rel in files:
        text = ctx.read(rel)
        if text is None:
            unreadable.append(rel)
            continue
        stripped = strip_comments_preserve_lines(text, tuple(ctx.spec.code_exts))
        guard_spans = _guarded_lines(stripped, guard_macro)
        for lineno, line in enumerate(stripped.splitlines(), start=1):
            for token in forbidden:
                if token in line:
                    if guard_spans.get(lineno):
                        continue
                    violations.append((rel, lineno, token, line.strip()[:120]))
            for token in allowed:
                if token in line:
                    allowed_hits.append((rel, lineno, token))

    if unreadable:
        findings.append(
            Finding(
                rule=rule.id, status=Status.WARN, subject="(扫描范围)",
                message=f"以下文件读取失败，STD-001 对其为盲区: {unreadable}",
                observed=len(unreadable), threshold=0, level="warning",
            )
        )

    for rel, lineno, token, snippet in violations[:10]:
        findings.append(
            Finding(
                rule=rule.id, status=Status.BLOCK, subject=f"{rel}:{lineno}", line=lineno,
                message=f"使用了交集外特性 {token}，且不在含 {guard_macro} 的 #if 保护块内：{snippet}",
                observed=token, threshold=f"必须包在 #if defined({guard_macro}...) / #endif 内",
                level="error",
                hint=(
                    f"二选一：① 删除 {token} 的用法；② 用 "
                    f"`#if defined({guard_macro}STD_XXX)` … `#else` 降级实现 … `#endif` 包住。"
                ),
            )
        )
    if len(violations) > 10:
        findings.append(
            Finding(
                rule=rule.id, status=Status.WARN, subject="(扫描范围)",
                message=f"另有 {len(violations) - 10} 处越界特性未逐条展开",
                observed=len(violations), threshold=10, level="warning",
            )
        )

    if violations:
        findings.append(
            Finding(
                rule=rule.id, status=Status.BLOCK, subject="(汇总)",
                message=(
                    f"越界特性 {len(violations)} 处 > 0；"
                    f"涉及记号 {sorted({v[2] for v in violations})}"
                ),
                observed=len(violations), threshold=0, level="error",
            )
        )
    else:
        findings.append(
            Finding(
                rule=rule.id, status=Status.PASS, subject="(汇总)",
                message=(
                    f"扫描 {len(files)} 个文件：越界特性 0 处"
                    + (f"；白名单命中 {len(allowed_hits)} 处" if allowed_hits else "；白名单记号未出现")
                ),
                observed=0, threshold=0, level="info",
            )
        )

    for rel, lineno, token in allowed_hits[:10]:
        findings.append(
            Finding(
                rule=rule.id, status=Status.PASS, subject=f"{rel}:{lineno}", line=lineno,
                message=f"白名单特性 {token}（四工具链交集内，允许直接使用）",
                observed=token, threshold="allowed", level="info",
            )
        )
    return findings


def _guarded_lines(text: str, guard_macro: str) -> dict[int, bool]:
    """返回 {行号: 是否处于含 guard_macro 的条件编译块内}。

    逐层维护 `#if/#ifdef/#ifndef` 栈：`#elif`/`#else` 会切换当前分支的"是否受保护"，
    `#endif` 出栈。这样 `#if FOO ... #else <越界特性> #endif` 不会被误判成已保护。
    """
    stack: list[bool] = []          # 每层当前分支是否含 guard_macro
    result: dict[int, bool] = {}
    for lineno, line in enumerate(text.splitlines(), start=1):
        m_if = _IF_RE.match(line)
        if m_if:
            cond = (m_if.group("cond") or "").strip()
            stack.append(guard_macro in cond)
            result[lineno] = any(stack)
            continue
        m_elif = _ELIF_RE.match(line)
        if m_elif:
            cond = (m_elif.group("cond") or "").strip()
            if stack:
                stack[-1] = guard_macro in cond
            result[lineno] = any(stack)
            continue
        if _ENDIF_RE.match(line):
            result[lineno] = any(stack)
            if stack:
                stack.pop()
            continue
        result[lineno] = any(stack)
    return result
