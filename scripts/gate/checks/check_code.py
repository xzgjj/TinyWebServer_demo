"""B 族：代码纪律（CODE-001 死代码与未使用符号 / CODE-002 重复代码块）。

只读实现；所有判定都建立在"这次改动涉及的文件"上，避免存量代码把门禁刷红。
可预期情况（文件读不出来、括号不配平）一律给 WARN/ERROR finding，不静默跳过。
"""
from __future__ import annotations

import hashlib
import re
from collections import defaultdict

from ..context import Context, normalize_code_line, strip_comments_preserve_lines
from ..model import Finding, Status
from ..spec import Rule
from . import glob_root

CHECKS = ("dead_code", "dup_code")


# --------------------------------------------------------------------------- #
# CODE-001 死代码与未使用符号
# --------------------------------------------------------------------------- #
_CODE_MARK_RE = re.compile(
    r";|\{|\}|->|::|\breturn\b|\bif\s*\(|\bfor\s*\(|\bwhile\s*\(|\bclass\b|\bstruct\b|\bvoid\b"
)
_PRIVATE_FIELD_RE = re.compile(r"\b([A-Za-z_]\w*_)\s*(?:=|;|\{|\[|\))")
# 只想抓"文件作用域 static 函数定义"，因此锚定 `static ... (` 的形态；
# 函数名从签名里取最后一个标识符（形如 `static void helper(` / `static T Cls::name(`）。
_STATIC_FUNC_RE = re.compile(r"(?m)^[ \t]*static\b(?P<sig>[^{;]*?)\(")
_SIGNATURE_NOISE = {
    "static", "inline", "constexpr", "consteval", "constinit", "const", "noexcept",
    "mutable", "volatile", "virtual", "explicit", "unsigned", "signed", "struct",
    "class", "typename", "auto", "void", "return", "operator",
}


def dead_code(ctx: Context, rule: Rule) -> list[Finding]:
    """只检查本次改动涉及的 C++ 文件：注释掉的代码块 / 未使用私有字段 / 未使用静态函数。"""
    findings: list[Finding] = []
    if not bool(rule.get("enabled", True)):
        return [
            Finding(
                rule=rule.id, status=Status.PASS, subject="dead_code",
                message="规则被 params.enabled=false 关闭（规则变更需 CI-001 审批）",
                observed=False, threshold=True, level="info",
            )
        ]

    files = ctx.code_files()
    if not files:
        return [
            Finding(
                rule=rule.id, status=Status.PASS, subject="(改动文件)",
                message="本次改动没有 C++ 文件，无需检查死代码",
                observed=0, threshold=1, level="info",
            )
        ]

    max_comment = int(rule.get("max_commented_block_lines", 12))
    max_fields = int(rule.get("max_unused_private_fields", 0))
    max_static = int(rule.get("max_unused_static_functions", 0))

    unreadable: list[str] = []
    commented_hits: list[tuple[str, int, int]] = []
    field_hits: list[tuple[str, str, int]] = []
    static_hits: list[tuple[str, str, int]] = []

    for rel in files:
        text = ctx.read(rel)
        if text is None:
            unreadable.append(rel)
            continue

        for start, end in _iter_comment_blocks(text):
            body = "\n".join(text.splitlines()[start - 1:end])
            span = max(1, end - start + 1)
            if span > max_comment and _CODE_MARK_RE.search(body):
                commented_hits.append((rel, start, span))

        stripped = strip_comments_preserve_lines(text, tuple(ctx.spec.code_exts))
        field_hits.extend(_unused_private_fields(ctx, rel, stripped))
        static_hits.extend(_unused_static_functions(ctx, rel, stripped))

    if unreadable:
        findings.append(
            Finding(
                rule=rule.id, status=Status.WARN, subject="(改动文件)",
                message=f"以下文件读取失败，未能检查死代码: {unreadable}",
                observed=len(unreadable), threshold=0,
                hint="确认文件权限/编码（UTF-8），否则本条规则对该文件是盲区。",
            )
        )

    for rel, start, span in commented_hits[:10]:
        findings.append(
            Finding(
                rule=rule.id, status=Status.BLOCK, subject=rel, line=start,
                message=f"连续被注释掉的代码块 {span} 行（> {max_comment}），起于第 {start} 行",
                observed=span, threshold=max_comment, level="error",
                hint="删除这段注释代码（Git 历史里已有），需要保留请改写成说明性注释。",
            )
        )
    if len(commented_hits) > 10:
        findings.append(
            Finding(
                rule=rule.id, status=Status.WARN, subject="(改动文件)",
                message=f"另有 {len(commented_hits) - 10} 处注释代码块未逐条展开",
                observed=len(commented_hits), threshold=10,
            )
        )

    if len(field_hits) > max_fields:
        for rel, name, line in field_hits[:10]:
            findings.append(
                Finding(
                    rule=rule.id, status=Status.BLOCK, subject=rel, line=line,
                    message=f"私有字段 {name} 声明后在全仓库找不到第二次引用（未使用）",
                    observed=len(field_hits), threshold=max_fields, level="error",
                    hint=f"删除 {name}，或补上真正使用它的代码（不要为了以后可能用而保留）。",
                )
            )
    if len(static_hits) > max_static:
        for rel, name, line in static_hits[:10]:
            findings.append(
                Finding(
                    rule=rule.id, status=Status.BLOCK, subject=rel, line=line,
                    message=f"static 函数 {name} 定义后全仓库无引用（未使用）",
                    observed=len(static_hits), threshold=max_static, level="error",
                    hint=f"删除 {name}，或补上调用点/对应单元测试。",
                )
            )

    if not commented_hits and len(field_hits) <= max_fields and len(static_hits) <= max_static:
        findings.append(
            Finding(
                rule=rule.id, status=Status.PASS, subject="(改动文件)",
                message=(
                    f"{len(files)} 个改动文件：注释代码块 0，未使用私有字段 {len(field_hits)}，"
                    f"未使用 static 函数 {len(static_hits)}"
                ),
                observed=0, threshold=f"块<={max_comment}, 字段<={max_fields}, 静态函数<={max_static}",
                level="info",
            )
        )
    return findings


def _iter_comment_blocks(text: str):
    """返回连续注释行块 [(首行, 末行)]（行注释与块注释合并计数）。"""
    lines = text.splitlines()
    spans: list[tuple[int, int]] = []
    start: int | None = None
    in_block = False
    for idx, line in enumerate(lines, start=1):
        s = line.strip()
        is_comment = False
        if in_block:
            is_comment = True
            if "*/" in line:
                in_block = False
        elif s.startswith("//"):
            is_comment = True
        elif s.startswith("/*"):
            is_comment = True
            if "*/" not in s[2:]:
                in_block = True
        if is_comment:
            if start is None:
                start = idx
        else:
            if start is not None:
                spans.append((start, idx - 1))
                start = None
    if start is not None:
        spans.append((start, len(lines)))
    return spans


def _unused_private_fields(ctx: Context, rel: str, stripped: str) -> list[tuple[str, str, int]]:
    """类内声明的 `xxx_` 字段：全仓库（含本文件）找不到第二次引用即视为未使用。"""
    hits: list[tuple[str, str, int]] = []
    seen: set[str] = set()
    for lineno, line in enumerate(stripped.splitlines(), start=1):
        if "(" in line and ")" in line and ";" not in line:
            continue                       # 函数参数/声明行，避免把形参当字段
        if line.lstrip().startswith(("//", "*")):
            continue
        for m in _PRIVATE_FIELD_RE.finditer(line):
            name = m.group(1)
            if name.endswith("__") or name in seen:
                continue
            seen.add(name)
            if _count_repo_occurrences(ctx, name) <= 1:
                hits.append((rel, name, lineno))
    return hits


def _unused_static_functions(ctx: Context, rel: str, stripped: str) -> list[tuple[str, str, int]]:
    """文件作用域的 `static <ret> name(` 定义：全仓库找不到第二次引用即视为未使用。"""
    hits: list[tuple[str, str, int]] = []
    for m in _STATIC_FUNC_RE.finditer(stripped):
        sig = re.sub(r"\[\[[^\]]*\]\]", " ", m.group("sig"))
        idents = [
            tok for tok in re.findall(r"[A-Za-z_]\w*", sig)
            if tok not in _SIGNATURE_NOISE
        ]
        if not idents:
            continue
        name = idents[-1]
        lineno = stripped.count("\n", 0, m.start()) + 1
        if _count_repo_occurrences(ctx, name) <= 1:
            hits.append((rel, name, lineno))
    return hits


def _count_repo_occurrences(ctx: Context, identifier: str) -> int:
    """全仓库 C++ 文件中该标识符出现次数（词边界匹配）。

    口径说明：跨文件统计是"宁可漏报也不误报"的取舍——头文件里的类内联实现也算引用，
    因此不会因为"调用点在 include/" 而误判成死代码。
    """
    exts = tuple(ctx.spec.code_exts)
    rx = re.compile(rf"\b{re.escape(identifier)}\b")
    total = 0
    for rel in _repo_code_files(ctx):
        if not rel.endswith(exts):
            continue
        text = ctx.read(rel)
        if text:
            total += len(rx.findall(text))
    return total


def _repo_code_files(ctx: Context) -> list[str]:
    key = "dead_code::repo_files"
    cached = ctx.cache.get(key)
    if isinstance(cached, list):
        return cached
    out: list[str] = []
    for pattern in ("src/**/*", "include/**/*", "reactor/**/*", "test/**/*", "benchmark/**/*"):
        out.extend(glob_root(ctx.root, pattern))
    files = sorted(set(out))
    ctx.cache[key] = files
    return files


# --------------------------------------------------------------------------- #
# CODE-002 重复代码块
# --------------------------------------------------------------------------- #
def dup_code(ctx: Context, rule: Rule) -> list[Finding]:
    """归一化行窗口指纹：重复窗口数 > max_dup_blocks -> WARN（spec 里 mode=warn）。"""
    min_lines = int(rule.get("min_lines", 8))
    max_dup = int(rule.get("max_dup_blocks", 2))
    files = ctx.code_files()
    if not files:
        return [
            Finding(
                rule=rule.id, status=Status.PASS, subject="(改动文件)",
                message="本次改动没有 C++ 文件，无需检查重复代码",
                observed=0, threshold=max_dup, level="info",
            )
        ]

    fingerprints: dict[str, list[tuple[str, int]]] = defaultdict(list)
    for rel in files:
        text = ctx.read(rel)
        if text is None:
            continue
        stripped = strip_comments_preserve_lines(text, tuple(ctx.spec.code_exts))
        norm = [normalize_code_line(ln) for ln in stripped.splitlines()]
        for idx in range(0, max(0, len(norm) - min_lines + 1)):
            window = [ln for ln in norm[idx: idx + min_lines] if ln]
            if len(window) < min_lines:
                continue                      # 窗口内有效行不足，不构成重复块
            digest = hashlib.sha1("\n".join(window).encode("utf-8")).hexdigest()
            fingerprints[digest].append((rel, idx + 1))

    dup_windows = {fp: locs for fp, locs in fingerprints.items() if len(locs) > 1}
    findings: list[Finding] = []
    if len(dup_windows) > max_dup:
        for fp, locs in list(dup_windows.items())[:10]:
            (fa, la), (fb, lb) = locs[0], locs[1]
            findings.append(
                Finding(
                    rule=rule.id, status=Status.WARN,
                    subject=f"{fa}:{la} <-> {fb}:{lb}",
                    message=(
                        f"发现重复代码窗口（{min_lines} 行归一化指纹一致）："
                        f"{fa}:{la} 与 {fb}:{lb}"
                    ),
                    observed=len(dup_windows), threshold=max_dup, level="warning",
                    line=la,
                    hint="抽出公共函数/模板，或说明两处为何必须独立（重复是【堆细节】的入口）。",
                )
            )
        findings.append(
            Finding(
                rule=rule.id, status=Status.WARN, subject="(改动文件)",
                message=f"重复窗口数 {len(dup_windows)} > {max_dup}",
                observed=len(dup_windows), threshold=max_dup, level="warning",
            )
        )
    else:
        findings.append(
            Finding(
                rule=rule.id, status=Status.PASS, subject="(改动文件)",
                message=f"重复窗口数 {len(dup_windows)} <= {max_dup}（扫描 {len(files)} 个改动文件）",
                observed=len(dup_windows), threshold=max_dup, level="info",
            )
        )
    return findings
