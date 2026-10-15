"""A 族：文档纪律（DOC-001 字数预算 / DOC-002 重复段落 / DOC-003 文档-代码一致性）。

设计要点：
  * 三个判定器共用一套"文档口径"：字数 = 非空白字符数；段落块 = 空行切分。
  * 只读：本模块只 `ctx.read()` / `ctx.exists()`，不产生任何写盘。
  * 可预期异常（文件缺失、编码坏、CMake 没写标准）一律给出明确 finding，
    禁止"环境不满足就假装通过"。
"""
from __future__ import annotations

import hashlib
import re
from collections import defaultdict
from typing import Any

from ..context import Context
from ..model import Finding, Status
from ..spec import Rule
from . import non_whitespace_chars, read_json_baseline, safe_join

CHECKS = ("doc_budget", "doc_dup", "doc_refs")


# --------------------------------------------------------------------------- #
# DOC-001 文档字数预算
# --------------------------------------------------------------------------- #
def doc_budget(ctx: Context, rule: Rule) -> list[Finding]:
    """预算内文档不得超限；存量文档按 ratchet 棘轮（只许减不许增）。

    ratchet 的比较对象是 gate/baseline.json 里记录的基线字数：
      current > baseline -> BLOCK；current <= baseline -> PASS（并记录当前值/基线）。
    基线缺失时按 ratchet 的本义处理——本次值即新基线，给 WARN（提醒补齐基线文件），
    绝不因为"没有基线"而放宽成永久通过。
    """
    findings: list[Finding] = []
    budgets: dict[str, int] = dict(rule.get("budgets") or {})
    grandfathered: list[str] = list(rule.get("grandfathered") or [])
    unlimited: list[str] = list(rule.get("unlimited_globs") or [])
    baseline_rel = str(ctx.spec.meta.get("baseline_path", "gate/baseline.json"))

    baseline, err = read_json_baseline(ctx, baseline_rel)
    if err is not None:
        findings.append(_with_rule(err, rule.id))
        baseline = None
    recorded: dict[str, Any] = {}
    if isinstance(baseline, dict):
        raw = baseline.get("doc_chars")
        if isinstance(raw, dict):
            recorded = raw
        elif baseline:
            findings.append(
                Finding(
                    rule=rule.id, status=Status.WARN, subject=baseline_rel,
                    message="基线文件存在但没有 doc_chars 段，ratchet 无法比较（视作首次观测）",
                    observed=sorted(baseline)[:8], threshold="含 doc_chars 的对象",
                    hint='运行门禁时带 --update-baseline 以补齐基线。',
                )
            )

    seen: set[str] = set()

    # ① 有预算的新文档：超限即阻断
    for name, limit in budgets.items():
        seen.add(name)
        text = ctx.read(name)
        if text is None:
            findings.append(
                Finding(
                    rule=rule.id, status=Status.PASS, subject=name,
                    message="未创建", observed=0, threshold=int(limit), level="info",
                )
            )
            continue
        count = non_whitespace_chars(text)
        limit_i = _as_int(limit, 0)
        if count > limit_i:
            findings.append(
                Finding(
                    rule=rule.id, status=Status.BLOCK, subject=name,
                    message=f"文档字数超预算：{count} > {limit_i}（口径 {rule.get('unit')}）",
                    observed=count, threshold=limit_i, level="error",
                    hint=f"精简 {name}：删除与代码重复的细节，把细节留在 {name} 之外的参考文档里。",
                )
            )
        else:
            findings.append(
                Finding(
                    rule=rule.id, status=Status.PASS, subject=name,
                    message=f"文档在预算内：{count} <= {limit_i}",
                    observed=count, threshold=limit_i, level="info",
                )
            )

    # ② 存量文档：豁免绝对上限，但不得增长（ratchet）
    for name in grandfathered:
        if name in seen:
            continue
        seen.add(name)
        text = ctx.read(name)
        if text is None:
            findings.append(
                Finding(
                    rule=rule.id, status=Status.PASS, subject=name,
                    message="未创建", observed=0, threshold="ratchet", level="info",
                )
            )
            continue
        count = non_whitespace_chars(text)
        base_val = _as_int(recorded.get(name), None)
        if base_val is None:
            findings.append(
                Finding(
                    rule=rule.id, status=Status.WARN, subject=name,
                    message=(
                        f"存量文档 {name} 无棘轮基线记录，本次 {count} 字按新基线处理；"
                        "请生成 gate/baseline.json 以免棘轮失效"
                    ),
                    observed=count, threshold="baseline.json::doc_chars",
                    hint="运行门禁时带 --update-baseline 生成基线，并把 gate/baseline.json 纳入版本控制。",
                )
            )
        elif count > base_val:
            findings.append(
                Finding(
                    rule=rule.id, status=Status.BLOCK, subject=name,
                    message=f"存量文档只许减不许增：{count} > 基线 {base_val}（增长 {count - base_val} 字）",
                    observed=count, threshold=base_val, level="error",
                    hint=f"把新增内容移到参考文档，或同步下调 {name}；确需增长须走 waiver（CI-001）。",
                )
            )
        else:
            findings.append(
                Finding(
                    rule=rule.id, status=Status.PASS, subject=name,
                    message=f"棘轮通过：当前 {count} <= 基线 {base_val}",
                    observed=count, threshold=base_val, level="info",
                )
            )

    # ③ 记录/分析类文档：不设上限，但必须留下统计（不是"跳过"）
    for pattern in unlimited:
        for name in _unlimited_targets(ctx, pattern):
            if name in seen:
                continue
            seen.add(name)
            text = ctx.read(name)
            count = non_whitespace_chars(text) if text else 0
            findings.append(
                Finding(
                    rule=rule.id, status=Status.PASS, subject=name,
                    message=f"记录/分析类文档，不设上限；当前 {count} 字（口径 {rule.get('unit')}）",
                    observed=count, threshold="unlimited", level="info",
                )
            )

    return findings


def _unlimited_targets(ctx: Context, pattern: str) -> list[str]:
    if "*" not in pattern:
        return [pattern]
    from . import glob_root

    return glob_root(ctx.root, pattern)


# --------------------------------------------------------------------------- #
# DOC-002 重复段落检查
# --------------------------------------------------------------------------- #
_DIGIT_RE = re.compile(r"\d+")
_BLOCK_SPLIT_RE = re.compile(r"\n[ \t]*\n+")


def doc_dup(ctx: Context, rule: Rule) -> list[Finding]:
    """段落块级重复：文档内重复 -> BLOCK；跨文档重复占比超阈值 -> BLOCK。

    归一化 = 去空白 + 数字→0 + sha1 指纹，保证"只改了数字的复制粘贴"也算重复。
    """
    findings: list[Finding] = []
    min_chars = int(rule.get("min_block_chars", 40))
    max_intra = int(rule.get("max_intra_dup", 0))
    max_cross = float(rule.get("max_cross_ratio", 0.05))

    docs = ctx.doc_candidates()
    if not docs:
        findings.append(
            Finding(
                rule=rule.id, status=Status.PASS, subject="(文档集)", message="未发现参与检查的文档",
                observed=0, threshold=min_chars, level="info",
            )
        )
        return findings

    intra_hits: list[Finding] = []
    per_doc_blocks: dict[str, list[tuple[str, int, str]]] = {}   # doc -> [(fp, line, head)]
    all_blocks: dict[str, list[str]] = defaultdict(list)         # fp -> [doc, ...]

    for name, text in docs.items():
        blocks: list[tuple[str, int, str]] = []
        for start, block in _iter_blocks(text):
            if non_whitespace_chars(block) < min_chars:
                continue
            fp = _fingerprint(block)
            head = " ".join(block.strip().split())[:80]
            blocks.append((fp, start, head))
            all_blocks[fp].append(name)
        per_doc_blocks[name] = blocks

        counts: dict[str, list[tuple[int, str]]] = defaultdict(list)
        for fp, line, head in blocks:
            counts[fp].append((line, head))
        for fp, occ in counts.items():
            dup_times = len(occ) - 1        # 第一次不算重复
            if dup_times > max_intra:
                first_line, head = occ[0]
                lines = ", ".join(str(ln) for ln, _ in occ[1:])
                intra_hits.append(
                    Finding(
                        rule=rule.id, status=Status.BLOCK, subject=name,
                        message=(
                            f"文档内重复段落：首行 {first_line}，重复出现在第 {lines} 行"
                            f"（共 {len(occ)} 次，允许 {max_intra} 次）"
                        ),
                        observed=dup_times, threshold=max_intra, level="error",
                        line=first_line,
                        hint=f"删除重复段落，或在 {name} 里改为引用出处（避免【堆细节】）。",
                    )
                )
    findings.extend(intra_hits[:20])            # 单文档重复最多报 20 条，避免刷屏
    if len(intra_hits) > 20:
        findings.append(
            Finding(
                rule=rule.id, status=Status.WARN, subject="(文档集)",
                message=f"另有 {len(intra_hits) - 20} 条文档内重复未逐条展开",
                observed=len(intra_hits), threshold=20, level="warning",
            )
        )

    total_blocks = sum(len(v) for v in per_doc_blocks.values())
    dup_blocks = sum(1 for fp, names in all_blocks.items() if len(set(names)) > 1)
    ratio = (dup_blocks / total_blocks) if total_blocks else 0.0
    if total_blocks and ratio > max_cross:
        samples = [
            (fp, sorted(set(names)))
            for fp, names in all_blocks.items()
            if len(set(names)) > 1
        ][:3]
        findings.append(
            Finding(
                rule=rule.id, status=Status.BLOCK, subject="(跨文档)",
                message=(
                    f"跨文档重复块占比 {ratio:.4f} > {max_cross}（重复块 {dup_blocks}/{total_blocks}）；"
                    f"样例: {[sorted(set(n)) for _, n in samples]}"
                ),
                observed=round(ratio, 4), threshold=max_cross, level="error",
                hint="同一段落只保留一处，其余改为链接引用。",
            )
        )
    else:
        findings.append(
            Finding(
                rule=rule.id, status=Status.PASS, subject="(跨文档)",
                message=f"跨文档重复块占比 {ratio:.4f} <= {max_cross}（重复块 {dup_blocks}/{total_blocks}）",
                observed=round(ratio, 4), threshold=max_cross, level="info",
            )
        )
    return findings


def _iter_blocks(text: str):
    """按空行切块，产出 (块首行号, 块文本)。"""
    line = 1
    for chunk in _BLOCK_SPLIT_RE.split(text):
        yield line, chunk
        line += chunk.count("\n") + 2      # 分隔符本身至少吃掉 2 个换行
    return


def _fingerprint(block: str) -> str:
    normalized = _DIGIT_RE.sub("0", "".join(block.split()))
    return hashlib.sha1(normalized.encode("utf-8")).hexdigest()


# --------------------------------------------------------------------------- #
# DOC-003 文档-代码一致性
# --------------------------------------------------------------------------- #
# 只认"仓库内路径"的形态：显式扩展名或仓库根目录前缀，避免把普通代码片段当路径
_PATH_EXTS = (
    ".cpp", ".cc", ".cxx", ".h", ".hpp", ".ipp", ".inl", ".md", ".txt", ".json",
    ".toml", ".yml", ".yaml", ".py", ".sh", ".cmake", ".html", ".css", ".js", ".iss",
)
_WHOLE_TOKEN_RE = re.compile(r"^[A-Za-z0-9_./+\-]+$")
_URL_SCHEME_RE = re.compile(r"^[A-Za-z][A-Za-z0-9+.\-]*:")


def doc_refs(ctx: Context, rule: Rule) -> list[Finding]:
    """文档里点名的仓库内文件必须真实存在；CLAUDE.md 声明的标准必须等于 CMake 实际默认标准。"""
    findings: list[Finding] = []
    docs: list[str] = list(rule.get("docs") or [])
    check_paths = bool(rule.get("check_paths", True))
    check_std = bool(rule.get("check_cxx_std", True))

    missing_docs: list[str] = []
    present_docs: list[str] = []
    for name in docs:
        if ctx.read(name) is None:
            missing_docs.append(name)
        else:
            present_docs.append(name)

    if check_paths:
        checked = 0
        for name in present_docs:
            text = ctx.read(name) or ""
            for token in sorted(_extract_path_tokens(ctx, text)):
                if not ctx.exists(token) and "/" not in token:
                    # 裸文件名（无目录前缀）：文档里可能只是提到一个概念/示例文件。
                    # 不存在时降级为 WARN，避免把"顺口提到的文件名"当成断链阻断。
                    findings.append(
                        Finding(
                            rule=rule.id, status=Status.WARN, subject=name,
                            message=f"文档提到的文件名在仓库内不存在（无法确认是否为仓库内路径）: {token}",
                            observed=token, threshold="存在或改写为明确路径", level="warning",
                            hint=f"若确指仓库内文件，请写成带目录的路径（如 src/{token}）或创建它。",
                        )
                    )
                    continue
                checked += 1
                if not ctx.exists(token):
                    findings.append(
                        Finding(
                            rule=rule.id, status=Status.BLOCK, subject=name,
                            message=f"文档引用的仓库内路径不存在: {token}",
                            observed=token, threshold="真实存在的仓库内路径", level="error",
                            hint=f"修正 {name} 中的引用，或创建 {token}；不要用不存在的路径点名。",
                        )
                    )
        findings.append(
            Finding(
                rule=rule.id, status=Status.PASS, subject="(路径引用)",
                message=f"已核对 {checked} 条仓库内路径引用，覆盖文档 {len(present_docs)} 个",
                observed=checked, threshold="全部存在", level="info",
            )
        )
        if missing_docs:
            findings.append(
                Finding(
                    rule=rule.id, status=Status.PASS, subject="(文档集)",
                    message=f"以下文档未创建，跳过路径核对: {missing_docs}",
                    observed=len(missing_docs), threshold="已存在文档", level="info",
                )
            )

    if check_std:
        findings.append(_check_cxx_std_consistency(ctx, rule))

    return findings


def _extract_path_tokens(ctx: Context, text: str) -> set[str]:
    """从反引号片段与 Markdown 链接目标里抽出"仓库内路径"候选。

    关键：反引号片段必须再按 ` 切开——`a.cpp` 与 `b.cpp` 可能写在同一个行内代码块里，
    整体当成一个 token 会既漏报（含反引号、判不出扩展名）又误报。
    """
    tokens: set[str] = set()
    for raw in re.findall(r"`([^`\n]+)`", text):
        for piece in raw.split("`"):
            tokens.add(piece.strip())
    for raw in re.findall(r"\]\(([^)\s]+)\)", text):
        tokens.add(raw.strip())

    rootdirs = {p.name for p in ctx.root.iterdir() if p.is_dir()} if ctx.root.is_dir() else set()
    out: set[str] = set()
    for token in tokens:
        if not token or any(ch in token for ch in "*?<>$|\\`"):
            continue
        if token.startswith(("http://", "https://", "#", "mailto:", "//")):
            continue
        if _URL_SCHEME_RE.match(token):          # 形如 cpp:23 / file:xx 的伪路径
            continue
        cand = token.split("#", 1)[0].strip().rstrip("/")
        if not cand or cand.startswith(("/", "~")) or ".." in cand.split("/"):
            continue
        if not _WHOLE_TOKEN_RE.match(cand):
            continue
        has_dir = "/" in cand
        looks_path = cand.endswith(_PATH_EXTS) or cand == "CMakeLists.txt" or has_dir
        if not looks_path:
            continue
        if has_dir and cand.split("/", 1)[0] not in rootdirs:
            # 目录当前不存在：仍然接受"可识别路径 + 已知扩展名"的写法。
            # 这恰恰是 DOC-003 要抓的情况（文档点名了已被删除/从未存在的源文件）；
            # 只接受带扩展名的形态，避免把普通代码片段（如 `foo/bar`）当作路径误报。
            if not cand.endswith(_PATH_EXTS):
                continue
        out.add(cand)
    return out


_STD_DECL_RE = re.compile(r"C\+\+\s*(?P<ver>\d{2})", re.IGNORECASE)


def _check_cxx_std_consistency(ctx: Context, rule: Rule) -> Finding:
    """比对 CMake 实际默认标准与 CLAUDE.md 声明的标准。"""
    cmake_text = ctx.read("CMakeLists.txt")
    if cmake_text is None:
        return Finding(
            rule=rule.id, status=Status.WARN, subject="CMakeLists.txt",
            message="CMakeLists.txt 不存在，无法核对 C++ 标准声明",
            observed=None, threshold="存在且声明 CMAKE_CXX_STANDARD",
            hint="确认仓库根目录是否有 CMakeLists.txt（本仓库的构建入口）。",
        )
    actual = _parse_default_cxx_standard(cmake_text)
    claude_text = ctx.read("CLAUDE.md")
    if claude_text is None:
        return Finding(
            rule=rule.id, status=Status.WARN, subject="CLAUDE.md",
            message="CLAUDE.md 不存在，无法比对声明的 C++ 标准（该文档受 DOC-001 预算约束）",
            observed=None, threshold="声明 C++ 标准",
        )
    declared_matches = _STD_DECL_RE.findall(claude_text)
    declared = f"c++{declared_matches[0]}" if declared_matches else None
    if actual is None or declared is None:
        return Finding(
            rule=rule.id, status=Status.WARN, subject="CLAUDE.md / CMakeLists.txt",
            message=(
                "无法完成 C++ 标准一致性核对："
                f"CMake 解析结果={actual!r}，CLAUDE.md 声明={declared!r}"
            ),
            observed={"cmake": actual, "claude_md": declared},
            threshold="两边都可解析",
            hint="在 CMakeLists.txt 里显式 set(CMAKE_CXX_STANDARD <NN>)，并在 CLAUDE.md 写明同一标准。",
        )
    if actual != declared:
        return Finding(
            rule=rule.id, status=Status.BLOCK, subject="CLAUDE.md / CMakeLists.txt",
            message=f"文档声明的 C++ 标准与 CMake 实际默认标准不一致：文档 {declared}，CMake {actual}",
            observed=declared, threshold=actual, level="error",
            hint="二者必须一致：改 CMakeLists.txt 的 CMAKE_CXX_STANDARD，或同步修正 CLAUDE.md 的声明。",
        )
    return Finding(
        rule=rule.id, status=Status.PASS, subject="CLAUDE.md / CMakeLists.txt",
        message=f"C++ 标准声明一致: {actual}",
        observed=actual, threshold=declared, level="info",
    )


_SET_RE = re.compile(
    r"(?m)^[ \t]*set\s*\(\s*(?P<var>CMAKE_CXX_STANDARD|TWS_CXX_STANDARD)\s+"
    r"(?P<val>\"[^\"]*\"|'[^']*'|\S+)",
    re.IGNORECASE,
)
_TWS_ASSIGN_RE = re.compile(
    r"(?m)^[ \t]*set\s*\(\s*(?P<var>TWS_CXX_STANDARD)\s+\"\s*(?P<val>\d+)\s*\"",
    re.IGNORECASE,
)
_CACHE_DECL_RE = re.compile(r"CACHE\s+(?:STRING|INTERNAL|BOOL)\b", re.IGNORECASE)
_VAR_REF_RE = re.compile(r"\$\{([^}]+)\}")


def _parse_default_cxx_standard(cmake_text: str) -> str | None:
    """解析 CMake 实际默认标准。

    口径（按优先级）：
      1. `TWS_CXX_STANDARD "23" CACHE STRING ...` 这类"缓存变量默认值"——它是用户可以
         覆盖的默认，也是文档声称能力的锚点；
      2. `set(TWS_CXX_STANDARD "...")` 的变量默认值（用于解析 `${TWS_CXX_STANDARD}`
         这类引用）；
      3. `set(CMAKE_CXX_STANDARD 20)` 的直接字面量；
      4. `-std=c++NN` 出现次数最多者。
    注释行一律剔除（注释里出现"17 / 20 / 23"是常态，不能当声明）。
    多个候选冲突时返回 None，让上层给出"不许假装通过"的 WARN。
    """
    body = "\n".join(ln for ln in cmake_text.splitlines()
                     if not ln.lstrip().startswith("#"))
    cache_values: list[str] = []
    plain_values: dict[str, str] = {}
    direct: list[str] = []
    for m in _SET_RE.finditer(body):
        raw = m.group("val").strip().strip('"').strip("'")
        if not raw or _VAR_REF_RE.search(raw):
            continue
        if _CACHE_DECL_RE.search(m.group(0)):
            cache_values.append(_norm_std(raw))
        else:
            plain_values[m.group("var").upper()] = _norm_std(raw)
    for m in _TWS_ASSIGN_RE.finditer(body):
        if _CACHE_DECL_RE.search(m.group(0)):
            continue
        plain_values.setdefault("TWS_CXX_STANDARD", _norm_std(m.group("val")))
    for m in re.finditer(r"\bset\s*\(\s*CMAKE_CXX_STANDARD\s+(\d+)\s*\)", body, re.IGNORECASE):
        direct.append(_norm_std(m.group(1)))

    candidates = [c for c in cache_values if c]
    if not candidates:
        for key in ("TWS_CXX_STANDARD", "CMAKE_CXX_STANDARD"):
            if plain_values.get(key):
                candidates.append(plain_values[key])
    if not candidates:
        candidates = direct
    if not candidates:
        flag_re = re.compile(r"-std=(?:gnu|c)\+\+(?P<ver>\d+)", re.IGNORECASE)
        seen: dict[str, int] = defaultdict(int)
        for m in flag_re.finditer(body):
            seen[f"c++{m.group('ver')}"] += 1
        if seen:
            candidates.append(max(seen.items(), key=lambda kv: kv[1])[0])

    uniq = {c for c in candidates if c}
    if len(uniq) == 1:
        return uniq.pop()
    return None


def _norm_std(value: str) -> str:
    v = value.strip().strip('"').lower()
    if v in {"latest", "on", "off"}:
        return v
    m = re.search(r"(?:c\+\+|gnu\+\+)?0*(\d+)", v)
    if m:
        return f"c++{m.group(1)}"
    return v


def _as_int(value: Any, default: int | None) -> int | None:
    try:
        return int(value)
    except (TypeError, ValueError):
        return default


def _with_rule(finding: Finding, rule_id: str) -> Finding:
    finding.rule = rule_id
    return finding
