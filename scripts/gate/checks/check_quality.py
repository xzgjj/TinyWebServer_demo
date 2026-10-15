"""D 族：质量纪律（QUAL-001 编译警告 / QUAL-002 复杂度 / QUAL-003 问题面板）。

三个判定器都把"环境不满足"当成必须报告的事实：
  * 编译器/cppcheck 缺失 -> WARN + 明确的安装/生成命令，绝不假装通过；
  * 超时 -> ERROR（检查自身没跑完，不能算通过）；
  * 解析不确定 -> 跳过该文件并在 finding 里说明，绝不误报。
"""
from __future__ import annotations

import json
import re
import shutil
import sys
import time
from pathlib import Path

from ..context import Context, strip_comments_preserve_lines
from ..model import Finding, Status
from ..spec import Rule
from . import code_source_set, evidence_path, glob_root, read_json_baseline, write_text_atomic

CHECKS = ("lint", "complexity", "problems_panel")

# 环境缺失时的安装指引（按平台给不同命令，避免给出跑不通的提示）
_PLATFORM_HINT = (
    "Windows：MSYS2/LLVM 安装后加入 PATH"
    if sys.platform.startswith("win")
    else "WSL/Ubuntu：sudo apt install g++ cppcheck"
)


# --------------------------------------------------------------------------- #
# QUAL-001 编译警告纪律
# --------------------------------------------------------------------------- #
# 诊断行解析：必须同时兼容 POSIX 与 Windows 盘符路径。
# `[^:\n]+` 会在 `C:\Users\...` 的第一个冒号处断开（Windows 上实测漏判所有警告），
# 因此用"贪婪但要求后面紧跟 :数字:数字:"的写法。
_DIAG_PREFIX = r"(?P<file>.+?):(?P<line>\d+):(?P<col>\d+):\s*"
_ERROR_RE = re.compile(rf"(?m)^{_DIAG_PREFIX}(?P<sev>error|fatal error):")
_WARN_RE = re.compile(rf"(?m)^{_DIAG_PREFIX}warning:")
_MSG_RE = re.compile(rf"(?m)^{_DIAG_PREFIX}(?P<sev>error|warning|fatal error|note):\s*(?P<msg>.*)$")

# cppcheck 专用：`--template={file}:{line}:{severity}:{message}` 没有列号，
# 且 severity 后面只有一个冒号，格式与编译器诊断不同，必须单独解析。
_CPPCHECK_RE = re.compile(
    r"(?m)^(?P<file>.+?):(?P<line>\d+):(?P<sev>[A-Za-z]+):(?P<msg>.*)$"
)
# 兜底：cppcheck 默认格式 `[file:line]: (severity) message`
_CPPCHECK_ALT_RE = re.compile(
    r"(?m)^\[(?P<file>.+?):(?P<line>\d+)\]:\s*\((?P<sev>[A-Za-z]+)\)\s*(?P<msg>.*)$"
)
_CPPCHECK_SEVERITIES = {"error", "warning", "style", "performance", "portability", "information"}


def lint(ctx: Context, rule: Rule) -> list[Finding]:
    """用本机可用的编译器做 `-fsyntax-only` 编译，错误数必须为 0，新增警告不得超基线。"""
    findings: list[Finding] = []
    compilers: list[str] = list(rule.get("compilers") or [])
    standard = str(rule.get("standard", "c++23"))
    flags: list[str] = list(rule.get("flags") or [])
    max_new_warnings = int(rule.get("max_new_warnings", 0))
    timeout_s = int(rule.get("timeout_s", 900))

    available = [(name, shutil.which(name)) for name in compilers]
    usable = [(name, path) for name, path in available if path]
    if not usable:
        return [
            Finding(
                rule=rule.id, status=Status.WARN, subject="(编译器)",
                message=(
                    f"params.compilers={compilers} 在本机全部不可用（shutil.which 未找到），"
                    "QUAL-001 无法编译验证——按未验证处理，不得当作通过"
                ),
                observed=[name for name, _ in available], threshold="至少一个可用编译器",
                hint="安装 g++（GCC）或 clang++，并在 PATH 中可用；Windows 可装 MSYS2/LLVM，WSL 用 apt install g++。",
            )
        ]

    targets = _lint_targets(ctx)
    if not targets:
        return [
            Finding(
                rule=rule.id, status=Status.WARN, subject="(源文件)",
                message="既无改动代码文件，也未在 src/include/reactor 下找到源文件，无可编译对象",
                observed=0, threshold=">=1", hint="确认 code_exts 配置与仓库布局。",
            )
        ]

    include_dirs = _guess_include_dirs(ctx)
    baseline_rel = str(ctx.spec.meta.get("baseline_path", "gate/baseline.json"))
    baseline, err = read_json_baseline(ctx, baseline_rel)
    if err is not None:
        findings.append(err)
        baseline = None
    warnings_baseline = None
    if isinstance(baseline, dict):
        raw = baseline.get("warnings")
        if isinstance(raw, dict):
            warnings_baseline = raw
    baseline_present = warnings_baseline is not None

    total_errors = 0
    total_warnings = 0
    peak_new_warnings = 0
    log_chunks: list[str] = []
    for name, path in usable:
        cmd = [path, "-fsyntax-only", "-x", "c++", f"-std={standard}", *flags]
        for inc in include_dirs:
            cmd += ["-I", inc]
        cmd += [str(ctx.root / rel) for rel in targets]
        started = time.time()
        proc = ctx.run(cmd, timeout=timeout_s)
        elapsed = time.time() - started
        out = (proc.stdout or "") + (proc.stderr or "")
        log_chunks.append(f"$ {' '.join(cmd)}\n# exit={proc.returncode} elapsed={elapsed:.1f}s\n{out}")

        if proc.returncode == 124:
            findings.append(
                Finding(
                    rule=rule.id, status=Status.ERROR, subject=name,
                    message=f"编译验证超时（{timeout_s}s）——检查未完成，不能判定通过",
                    observed=f"{timeout_s}s", threshold=f"< {timeout_s}s", level="error",
                    hint="缩小目标文件范围，或调大 params.timeout_s（规则变更需 CI-001 审批）。",
                )
            )
            continue
        if proc.returncode == 127:
            findings.append(
                Finding(
                    rule=rule.id, status=Status.ERROR, subject=name,
                    message=f"无法执行编译器 {path}: {proc.stderr.strip()[:200]}",
                    observed=proc.returncode, threshold=0, level="error",
                )
            )
            continue

        errors = len(_ERROR_RE.findall(out))
        warns = len(_WARN_RE.findall(out))
        total_errors += errors
        total_warnings += warns

        base_val = None
        if baseline_present:
            raw_val = warnings_baseline.get(name)
            try:
                base_val = int(raw_val)
            except (TypeError, ValueError):
                base_val = None
        new_warnings = warns - (base_val or 0)
        peak_new_warnings = max(peak_new_warnings, new_warnings)

        messages = list(_MSG_RE.finditer(out))
        if errors:
            for m in messages:
                sev = m.group("sev")
                if sev not in ("error", "fatal error"):
                    continue
                findings.append(
                    Finding(
                        rule=rule.id, status=Status.BLOCK,
                        subject=f"{_rel_of(ctx, m.group('file'))}:{m.group('line')}",
                        message=f"[{name}] 编译错误: {m.group('msg').strip()[:200]}",
                        observed=errors, threshold=0, level="error",
                        line=_int(m.group("line")),
                        hint="先修编译错误，门禁不接受「编译不过但测试能跑」的改动。",
                    )
                )
                break                        # 每个编译器先暴露第一条错误，避免刷屏
        if new_warnings > max_new_warnings:
            samples = [
                f"{_rel_of(ctx, m.group('file'))}:{m.group('line')}: {m.group('msg').strip()[:160]}"
                for m in messages
                if m.group("sev") == "warning"
            ][:5]
            findings.append(
                Finding(
                    rule=rule.id, status=Status.BLOCK, subject=name,
                    message=(
                        f"[{name}] 新增警告 {new_warnings} 个 > 允许 {max_new_warnings}"
                        f"（当前 {warns}，基线 {base_val}）"
                    ),
                    observed=new_warnings, threshold=max_new_warnings, level="error",
                    hint="消除新增警告；若基线本就偏低，修正 gate/baseline.json 的 warnings 段（需 CI-001 审批）。",
                )
            )
            for s in samples:
                findings.append(
                    Finding(
                        rule=rule.id, status=Status.WARN, subject=s.split(":")[0],
                        message=f"[{name}] 警告样例: {s}",
                        observed="warning", threshold=0, level="warning",
                    )
                )

    # 证据：完整编译器日志（供复核）
    evidence_rel: str | None = None
    try:
        abs_path, evidence_rel = evidence_path(ctx, "lint")
        write_text_atomic(abs_path, "\n".join(log_chunks))
    except OSError as exc:
        findings.append(
            Finding(
                rule=rule.id, status=Status.WARN, subject="(证据文件)",
                message=f"编译日志证据写出失败: {exc}",
                observed=type(exc).__name__, threshold="可写 local/gate/<run_id>/evidence/",
            )
        )

    if not baseline_present:
        findings.append(
            Finding(
                rule=rule.id, status=Status.WARN, subject=baseline_rel,
                message=(
                    f"缺少警告基线（{baseline_rel} 无 warnings 段），本次按基线 0 判定；"
                    f"当前警告数已写入证据，请生成基线"
                ),
                observed=total_warnings, threshold="含 warnings 段的基线文件",
                evidence=evidence_rel,
                hint="运行门禁时带 --update-baseline，把当前警告数固化为基线。",
            )
        )

    if total_errors == 0 and peak_new_warnings <= max_new_warnings:
        findings.append(
            Finding(
                rule=rule.id, status=Status.PASS, subject="(编译)",
                message=(
                    f"{len(targets)} 个文件以 {standard} 语法检查通过"
                    f"（{', '.join(n for n, _ in usable)}）：错误 0，警告 {total_warnings}"
                ),
                observed=total_warnings,
                threshold=f"错误=0 且新增警告<={max_new_warnings}",
                level="info", evidence=evidence_rel,
            )
        )
    return findings


def _lint_targets(ctx: Context) -> list[str]:
    """编译验证的对象集：**真实构建参与编译的文件**（不是"只看改动文件"）。

    为什么这样定：
      1) 用编译数据库而不是"猜"目标集：Windows 只构建可移植核心（SPEC 第 2 节平台策略），
         若门禁硬去编译网络层，会把"本就不构建"的代码报成编译错误 —— 纯误报；
      2) 目标集必须稳定：QUAL-001 是警告棘轮，目标集随改动漂移会让基线失去意义。

    编译数据库不可用时退回"仓库布局 + 平台过滤"，并明确在 finding 里说明。
    默认排除 `test/**`（测试依赖项目自己的链接配置，裸编译会误报缺 gtest 头）。
    """
    from ..context import match_any

    cc_files = _compile_commands_files(ctx)
    if cc_files:
        return [rel for rel in cc_files if not rel.startswith("test/")]

    patterns = [str(p) for p in (ctx.spec.rule("QUAL-001").get("compile_globs") or [])]
    if not patterns:
        # 注意 `src/**/*` 不匹配 `src/a.cpp`（`**/` 需要至少一层目录），必须补 `src/*`
        patterns = ["src/**/*", "src/*", "reactor/**/*", "reactor/*",
                    "include/**/*", "include/*"]
    files = [rel for rel in code_source_set(ctx) if match_any(rel, patterns)]
    if sys.platform.startswith("win"):
        files = [rel for rel in files if not _is_network_source(rel)]
    return sorted(files)


# Windows 上不构建的网络层（与 CMakeLists.txt 的 NETWORK_SOURCES 对应）
_NETWORK_SOURCE_PREFIXES = (
    "src/server.cpp", "src/connection.cpp", "src/static_resource_manager.cpp",
    "reactor/", "src/http2/", "src/plugin/", "src/main.cpp",
)


def _is_network_source(rel: str) -> bool:
    return any(rel == p or rel.startswith(p) for p in _NETWORK_SOURCE_PREFIXES)


def _static_analysis_targets(ctx: Context) -> list[str]:
    """静态分析对象集。

    与 `_lint_targets` 同一取舍：默认只看 `src/** reactor/** include/**`，
    因为 `test/**` 需要项目自己的链接/包含配置，裸跑 cppcheck 会把测试代码的
    正常写法（如常量折叠后的恒真判断）报成问题，属误报来源。
    需要覆盖时用 `params.compile_globs` 显式指定。
    """
    return _lint_targets(ctx)


def _compile_commands_entries(ctx: Context) -> list[dict]:
    """收集本机平台匹配的 compile_commands.json 条目（按平台优先）。"""
    candidates: list[Path] = []
    for pattern in ("build*/compile_commands.json", "build*/*/compile_commands.json"):
        candidates.extend(sorted(ctx.root.glob(pattern)))

    fallback: list[dict] = []
    for cc in candidates:
        try:
            data = json.loads(cc.read_text(encoding="utf-8", errors="replace"))
        except (OSError, json.JSONDecodeError):
            continue
        if not isinstance(data, list) or not data:
            continue
        directory = data[0].get("directory") if isinstance(data[0], dict) else None
        same_platform = not directory or Path(str(directory)).is_dir()
        if same_platform:
            return [e for e in data if isinstance(e, dict)]
        if not fallback:
            fallback = [e for e in data if isinstance(e, dict)]
    return fallback


def _compile_commands_files(ctx: Context) -> list[str]:
    """compile_commands.json 里真实参与编译的源文件（仓库相对路径）。"""
    files: list[str] = []
    for entry in _compile_commands_entries(ctx):
        raw = entry.get("file")
        if not isinstance(raw, str):
            continue
        rel = _rel_of(ctx, raw)
        if rel.endswith(tuple(ctx.spec.code_exts)) and rel not in files:
            files.append(rel)
    return sorted(files)


def _compile_commands_dirs(ctx: Context) -> list[str]:
    """从构建产出的 compile_commands.json 提取真实 include 目录。

    为什么优先用它：门禁若自己"猜"-I，就会与真实构建脱节。compile_commands.json
    是构建系统给出的权威参数（平台差异、兼容头层、宏定义都在里面）。

    多平台共用同一工作区时（同时存在 build/ 与 build/matrix-*/），必须挑
    "属于本机平台的那一份"：判据是条目里的 directory 字段在本机确实存在。
    """
    fallback: list[str] = []
    for entry in _compile_commands_entries(ctx):
        tokens = entry.get("arguments")
        if not isinstance(tokens, list):
            # 路径不含空格时 split() 足够；含空格的项目应改用 arguments 形式
            tokens = str(entry.get("command", "")).split()
        dirs: list[str] = []
        for tok in tokens:
            if isinstance(tok, str) and tok.startswith("-I") and len(tok) > 2:
                if tok[2:] not in dirs:
                    dirs.append(tok[2:])
        if not dirs:
            continue
        directory = entry.get("directory")
        if not directory or Path(str(directory)).is_dir():
            return dirs[:12]
        if not fallback:
            fallback = dirs[:12]
    return fallback


def _guess_include_dirs(ctx: Context) -> list[str]:
    """include 目录（-I）来源优先级：

    1. build*/compile_commands.json：真实构建参数（含平台兼容头层）；
    2. 仓库布局推断：Windows 必须把 include/platform/win32 前置，
       否则 POSIX 头缺失会被误判成编译错误（实测于 MSYS2）。
    """
    from_cc = _compile_commands_dirs(ctx)
    if from_cc:
        return from_cc

    dirs: list[str] = []
    if sys.platform.startswith("win"):
        win_layer = ctx.root / "include" / "platform" / "win32"
        if win_layer.is_dir():
            dirs.append(str(win_layer))
    if (ctx.root / "include").is_dir():
        dirs.append(str(ctx.root / "include"))
    if not (ctx.root / "third_party").is_dir():
        return dirs
    for pattern in ("third_party/**/single_include", "third_party/**/include", "third_party/*"):
        for rel in glob_root(ctx.root, pattern):
            p = ctx.root / rel
            if p.is_dir():
                dirs.append(str(p))
        if len(dirs) > 1:
            break
    return dirs[:6]


def _rel_of(ctx: Context, path: str) -> str:
    try:
        return Path(path).resolve().relative_to(ctx.root.resolve()).as_posix()
    except (OSError, ValueError):
        return path


def _int(value: str) -> int | None:
    try:
        return int(value)
    except (TypeError, ValueError):
        return None


# --------------------------------------------------------------------------- #
# QUAL-002 复杂度上限
# --------------------------------------------------------------------------- #
CONTROL_KEYWORDS = {
    "if", "for", "while", "switch", "catch", "return", "sizeof", "alignof",
    "decltype", "static_assert", "do", "else", "new", "delete", "throw", "requires",
    "noexcept", "explicit", "operator", "typeid", "static_cast", "const_cast",
    "reinterpret_cast", "dynamic_cast", "case", "default", "try", "co_await", "co_return",
}

# `{` 紧跟在这些关键字之后时，是控制块而不是函数体
_CTRL_WORDS = {"if", "for", "while", "switch", "catch", "else", "do", "try"}


def complexity(ctx: Context, rule: Rule) -> list[Finding]:
    """轻量 C++ 解析：括号配平取函数体，统计行数/嵌套深度/圈复杂度。"""
    findings: list[Finding] = []
    max_cyclomatic = int(rule.get("max_cyclomatic", 15))
    max_function_lines = int(rule.get("max_function_lines", 80))
    max_nesting = int(rule.get("max_nesting", 4))
    scope_mode = str(rule.get("scope", "changed_files"))

    if scope_mode != "changed_files":
        findings.append(
            Finding(
                rule=rule.id, status=Status.WARN, subject="(scope)",
                message=f"params.scope={scope_mode!r} 不是 changed_files：仅对改动文件生效是设计约定",
                observed=scope_mode, threshold="changed_files", level="warning",
            )
        )
    files = ctx.affected_matching([f"**/*{ext}" for ext in ctx.spec.code_exts])
    if not files:
        return findings + [
            Finding(
                rule=rule.id, status=Status.PASS, subject="(改动文件)",
                message="本次改动没有 C++ 文件，复杂度检查无对象",
                observed=0, threshold=max_cyclomatic, level="info",
            )
        ]

    skipped: list[tuple[str, str]] = []
    hits: list[tuple[str, str, int, int, int, int]] = []
    checked = 0
    for rel in files:
        text = ctx.read(rel)
        if text is None:
            skipped.append((rel, "文件读取失败"))
            continue
        stripped = strip_comments_preserve_lines(text, tuple(ctx.spec.code_exts))
        funcs, reason = _parse_functions(stripped, rel)
        if reason:
            skipped.append((rel, reason))
            continue
        checked += 1
        for fn in funcs:
            if fn.cyclomatic > max_cyclomatic or fn.lines > max_function_lines or fn.nesting > max_nesting:
                hits.append((rel, fn.name, fn.line, fn.cyclomatic, fn.lines, fn.nesting))

    if skipped:
        findings.append(
            Finding(
                rule=rule.id, status=Status.WARN, subject="(跳过文件)",
                message=f"以下文件解析不确定，已跳过（不误报）: {[f'{p}({r})' for p, r in skipped]}",
                observed=len(skipped), threshold=0, level="warning",
                hint="若确需覆盖这些文件，请检查是否有宏拼接/非标准扩展；本 check 宁可跳过也不误报。",
            )
        )

    # 存量棘轮：已超标但"不比基线更差"的函数冻结在 gate/baseline.json，
    # 只允许下降不允许恶化。没有棘轮时，任何一次触碰遗留文件都会让门禁永久变红，
    # 结果必然是有人去关规则 —— 这正是要避免的。
    baseline_rel = str(ctx.spec.meta.get("baseline_path", "gate/baseline.json"))
    baseline, berr = read_json_baseline(ctx, baseline_rel)
    complexity_baseline: dict[str, dict] = {}
    if isinstance(baseline, dict) and isinstance(baseline.get("complexity"), dict):
        complexity_baseline = {
            str(k): v for k, v in baseline["complexity"].items() if isinstance(v, dict)
        }
    if berr is not None and hits:
        findings.append(berr)

    frozen: list[str] = []
    for rel, name, line, cyc, fn_lines, nesting in hits:
        key = f"{rel}:{name}"
        prev = complexity_baseline.get(key)
        if prev is not None and _not_worse(prev, cyc, fn_lines, nesting):
            frozen.append(f"{key} {cyc}/{fn_lines}/{nesting}")
            findings.append(
                Finding(
                    rule=rule.id, status=Status.WARN, subject=f"{rel}:{line}",
                    message=(
                        f"存量超标函数 {name} 已冻结在基线（当前 圈复杂度 {cyc}、{fn_lines} 行、"
                        f"嵌套 {nesting}）：只允许下降，不允许恶化"
                    ),
                    observed={"cyclomatic": cyc, "lines": fn_lines, "nesting": nesting},
                    threshold=prev, level="warning", line=line,
                    hint="拆分该函数后收紧 gate/baseline.json 的 complexity 段（规则文件变更需 CI-001 审批）。",
                )
            )
            continue
        reasons = []
        if cyc > max_cyclomatic:
            reasons.append(f"圈复杂度 {cyc} > {max_cyclomatic}")
        if fn_lines > max_function_lines:
            reasons.append(f"函数 {fn_lines} 行 > {max_function_lines}")
        if nesting > max_nesting:
            reasons.append(f"嵌套深度 {nesting} > {max_nesting}")
        findings.append(
            Finding(
                rule=rule.id, status=Status.BLOCK, subject=f"{rel}:{line}",
                message=f"函数 {name} 超限：" + "；".join(reasons),
                observed={"cyclomatic": cyc, "lines": fn_lines, "nesting": nesting},
                threshold={
                    "max_cyclomatic": max_cyclomatic,
                    "max_function_lines": max_function_lines,
                    "max_nesting": max_nesting,
                },
                level="error", line=line,
                hint="拆函数：早返回替代嵌套、把分支抽成具名小函数（不要靠注释掩盖复杂度）。",
            )
        )

    if not hits or len(frozen) == len(hits):
        findings.append(
            Finding(
                rule=rule.id, status=Status.PASS, subject="(改动文件)",
                message=(
                    f"{checked}/{len(files)} 个改动文件解析成功，无新增超限函数"
                    f"（圈复杂度<={max_cyclomatic}，行长<={max_function_lines}，嵌套<={max_nesting}）"
                    + (f"；存量冻结 {len(frozen)} 个" if frozen else "")
                ),
                observed=len(frozen), threshold=max_cyclomatic, level="info",
            )
        )
    return findings


def _not_worse(prev: dict, cyc: int, lines: int, nesting: int) -> bool:
    """当前指标是否不比基线更差（基线缺字段时按"当前值"视为不恶化）。"""
    for key, now in (("cyclomatic", cyc), ("lines", lines), ("nesting", nesting)):
        try:
            base = int(prev.get(key, now))
        except (TypeError, ValueError):
            continue
        if now > base:
            return False
    return True


class _Func:
    __slots__ = ("name", "line", "lines", "nesting", "cyclomatic")

    def __init__(self, name: str, line: int, lines: int, nesting: int, cyclomatic: int) -> None:
        self.name = name
        self.line = line
        self.lines = lines
        self.nesting = nesting
        self.cyclomatic = cyclomatic


_TOKEN_RE = re.compile(
    r"//[^\n]*|/\*.*?\*/|\"(?:\\.|[^\"\\])*\"|'(?:\\.|[^'\\])*'|(?P<id>[A-Za-z_]\w*)|(?P<op>&&|\|\||\?\?|->|::|[{}\[\]();,])",
    re.DOTALL,
)


def _parse_functions(stripped: str, rel: str) -> tuple[list[_Func], str | None]:
    """返回 (函数列表, 跳过原因)。括号不配平/字符串未闭合 -> 跳过，不误报。"""
    tokens: list[tuple[str, str, int]] = []
    for m in _TOKEN_RE.finditer(stripped):
        line = stripped.count("\n", 0, m.start()) + 1
        if m.group("id"):
            tokens.append(("id", m.group("id"), line))
        elif m.group("op"):
            tokens.append(("op", m.group("op"), line))
    if stripped.count("{") != stripped.count("}"):
        return [], "花括号数量不配平（可能有宏拼接或非标准扩展）"

    funcs: list[_Func] = []
    depth = 0
    pending: list[tuple[str, int]] = []         # 当前层的签名 token（成对括号已折叠为 ')'）
    stack: list[dict | None] = []               # 每层大括号对应的"函数上下文"，非函数为 None
    ctrl_parens = False                         # 本层 `{` 前是否跟在控制关键字后面
    paren_depth = 0
    for kind, val, ln in tokens:
        if kind == "op" and val == "(":
            if depth == 0:
                pending.append((val, ln))
                paren_depth += 1
            continue
        if kind == "op" and val == ")":
            if depth == 0:
                paren_depth -= 1
                while pending and pending[-1][0] not in {"(", ")"}:
                    pending.pop()
                if pending and pending[-1][0] == "(":
                    pending.pop()                # 参数列表整体折叠掉：名字只看 '(' 之前
                pending.append((")", ln))
                paren_depth = max(0, paren_depth)
            continue
        if kind == "op" and val == "{":
            name = None
            if depth == 0 and not ctrl_parens and paren_depth == 0 and _looks_like_definition(pending):
                name = _signature_name(pending)
            env = (
                {"name": name, "line": ln, "cyc": 1, "depth": 1, "max_depth": 1}
                if name
                else None
            )
            parent = _nearest_env(stack)         # 本层的父函数上下文（可能是 None）
            if parent is not None:
                parent["depth"] += 1
                parent["max_depth"] = max(parent["max_depth"], parent["depth"])
            stack.append(env)
            depth += 1
            pending = []
            ctrl_parens = False
            continue
        if kind == "op" and val == "}":
            depth -= 1
            if depth < 0:
                return [], "大括号配平穿越（文件结构异常）"
            env = stack.pop() if stack else None
            if env is not None:
                funcs.append(
                    _Func(
                        name=env["name"], line=env["line"],
                        lines=max(1, ln - env["line"] + 1),
                        nesting=env["max_depth"], cyclomatic=env["cyc"],
                    )
                )
            parent = _nearest_env(stack)
            if parent is not None:
                parent["depth"] = max(1, parent["depth"] - 1)
            pending = []
            ctrl_parens = False
            continue
        if kind == "op" and val == ";" and depth == 0:
            pending = []                        # 声明结束，清空签名缓存
            ctrl_parens = False
            continue
        if kind == "id":
            if depth == 0:
                pending.append((val, ln))
                if val in _CTRL_WORDS:
                    ctrl_parens = True
                continue
            env = _nearest_env(stack)
            if env is not None and val in {"if", "for", "while", "case", "catch"}:
                env["cyc"] += 1
            continue
        if kind == "op" and val in {"&&", "||", "??", "?"}:
            env = _nearest_env(stack)
            if env is not None:
                env["cyc"] += 1
            continue
    if stack:
        return [], "大括号未闭合"
    return funcs, None


def _nearest_env(stack: list[dict | None]) -> dict | None:
    for env in reversed(stack):
        if env is not None:
            return env
    return None


def _signature_name(pending: list[tuple[str, int]]) -> str | None:
    """从签名 token 序列里取函数名（参数列表之前最后一个合法标识符）。"""
    names = [tok for tok, _ in pending if tok not in CONTROL_KEYWORDS and tok != ")"]
    if not names:
        return None
    candidate = names[-1]
    if candidate in CONTROL_KEYWORDS:
        return None
    return candidate


def _looks_like_definition(pending: list[tuple[str, int]]) -> bool:
    """`{` 之前的签名必须"参数列表已闭合"且不是初始化列表/类型定义。"""
    if not pending:
        return False
    if pending[-1][0] != ")":                 # 必须形如 `name(...) {`
        return False
    words = {tok for tok, _ in pending}
    if words & {"struct", "class", "union", "enum", "namespace", "typedef", "using"}:
        return False
    return True


# --------------------------------------------------------------------------- #
# QUAL-003 编辑器问题面板为零
# --------------------------------------------------------------------------- #
def problems_panel(ctx: Context, rule: Rule) -> list[Finding]:
    """compile_commands.json 必须存在且可用；有 cppcheck 就跑，没跑成必须说清楚。"""
    findings: list[Finding] = []
    require_cc = bool(rule.get("require_compile_commands", True))
    cppcheck_enabled = bool(rule.get("cppcheck_enable", True))
    max_errors = int(rule.get("cppcheck_max_errors", 0))

    cc_path = ctx.root / "build" / "compile_commands.json"
    cc_ok = False
    if not cc_path.is_file():
        status = Status.BLOCK if require_cc else Status.WARN
        findings.append(
            Finding(
                rule=rule.id, status=status, subject="build/compile_commands.json",
                message="compile_commands.json 不存在：clangd/cppcheck 无法按真实编译参数解析，Problems 面板不作数",
                observed=False, threshold=True, level="error" if require_cc else "warning",
                hint=(
                    "生成命令: cmake -S . -B build -DCMAKE_EXPORT_COMPILE_COMMANDS=ON "
                    "（CMAKE_EXPORT_COMPILE_COMMANDS 只对 Makefile/Ninja 生成器生效）"
                ),
            )
        )
    else:
        text = ctx.read("build/compile_commands.json")
        try:
            data = json.loads(text or "")
        except json.JSONDecodeError as exc:
            data = None
            findings.append(
                Finding(
                    rule=rule.id, status=Status.BLOCK, subject="build/compile_commands.json",
                    message=f"compile_commands.json 不是合法 JSON: {exc}",
                    observed="invalid json", threshold="JSON 数组", level="error",
                    hint="删除后重新生成: cmake -S . -B build -DCMAKE_EXPORT_COMPILE_COMMANDS=ON",
                )
            )
        if isinstance(data, list) and data:
            cc_ok = True
        elif isinstance(data, list):
            findings.append(
                Finding(
                    rule=rule.id, status=Status.BLOCK, subject="build/compile_commands.json",
                    message="compile_commands.json 为空数组（没有任何编译单元）",
                    observed=0, threshold=">=1 条编译命令", level="error",
                    hint="确认构建目录里真的编译了源文件（至少 src/main.cpp）。",
                )
            )
        elif data is not None:
            findings.append(
                Finding(
                    rule=rule.id, status=Status.BLOCK, subject="build/compile_commands.json",
                    message="compile_commands.json 顶层不是 JSON 数组",
                    observed=type(data).__name__, threshold="array", level="error",
                )
            )

    if cc_ok:
        findings.append(
            Finding(
                rule=rule.id, status=Status.PASS, subject="build/compile_commands.json",
                message="compile_commands.json 存在且可解析（clangd 可按真实参数解析）",
                observed=True, threshold=True, level="info",
            )
        )
        # 轻量一致性：依赖的 include 目录是否真实存在（-I 指向不存在的目录 = 面板会红）
        dirs = _extract_include_dirs(ctx.read("build/compile_commands.json") or "")
        missing = [d for d in dirs if not (ctx.root / d).exists()]
        if missing:
            findings.append(
                Finding(
                    rule=rule.id, status=Status.WARN, subject="build/compile_commands.json",
                    message=f"compile_commands 里的 -I 目录不存在: {missing[:5]}",
                    observed=missing[:5], threshold="全部存在", level="warning",
                    hint="改动目录结构后需要重新生成 compile_commands.json。",
                )
            )

    if not cppcheck_enabled:
        findings.append(
            Finding(
                rule=rule.id, status=Status.WARN, subject="cppcheck",
                message="params.cppcheck_enable=false：静态分析未执行（规则变更需 CI-001 审批）",
                observed=False, threshold=True, level="warning",
            )
        )
        return findings

    exe = shutil.which("cppcheck")
    if not exe:
        findings.append(
            Finding(
                rule=rule.id, status=Status.WARN, subject="cppcheck",
                message="本机未找到 cppcheck，QUAL-003 的静态分析部分未执行（按未验证处理）",
                observed=None, threshold="cppcheck 可执行文件",
                hint=f"安装 cppcheck（{_PLATFORM_HINT}）。",
            )
        )
        return findings

    targets = _static_analysis_targets(ctx)
    if not targets:
        findings.append(
            Finding(
                rule=rule.id, status=Status.WARN, subject="cppcheck",
                message="无可分析文件（改动为空且核心源集为空）",
                observed=0, threshold=">=1",
            )
        )
        return findings

    cmd = [
        exe, "--enable=warning,style", "--error-exitcode=1", "--quiet",
        "--template={file}:{line}:{severity}:{message}",
        # cppcheck 对 .h 会按 C 解析（实测会把 `namespace x {` 报成 invalid C code），
        # 头文件全是 C++，必须显式指定语言，否则产出的是假问题。
        "--language=c++",
        "--inline-suppr",
        *[str(ctx.root / rel) for rel in targets],
    ]
    timeout_s = int(rule.get("timeout_s", 900) or 900)
    proc = ctx.run(cmd, timeout=timeout_s)
    if proc.returncode == 124:
        findings.append(
            Finding(
                rule=rule.id, status=Status.ERROR, subject="cppcheck",
                message=f"cppcheck 超时（{timeout_s}s），静态分析未完成",
                observed=f"{timeout_s}s", threshold=f"< {timeout_s}s", level="error",
            )
        )
        return findings
    if proc.returncode == 127:
        findings.append(
            Finding(
                rule=rule.id, status=Status.ERROR, subject="cppcheck",
                message=f"cppcheck 无法执行: {proc.stderr.strip()[:200]}",
                observed=proc.returncode, threshold=0, level="error",
            )
        )
        return findings

    out = (proc.stdout or "") + (proc.stderr or "")
    # cppcheck 的 severity 除了 error/warning 还有 style/performance/portability，
    # 这些都是"问题面板里会出现的问题"，会计入 cppcheck_max_errors；
    # 只把纯信息性的 information 排除在外。
    severity_counts: dict[str, int] = {}
    diags: list[str] = []
    for m in _CPPCHECK_RE.finditer(out):
        sev = m.group("sev")
        if sev not in _CPPCHECK_SEVERITIES or sev == "information":
            continue
        severity_counts[sev] = severity_counts.get(sev, 0) + 1
        diags.append(m.group(0))
    if not diags:
        for m in _CPPCHECK_ALT_RE.finditer(out):
            sev = m.group("sev")
            if sev not in _CPPCHECK_SEVERITIES:
                continue
            severity_counts[sev] = severity_counts.get(sev, 0) + 1
            diags.append(m.group(0))
    errors = len(diags)

    evidence_rel: str | None = None
    try:
        abs_path, evidence_rel = evidence_path(ctx, "cppcheck")
        write_text_atomic(abs_path, f"$ {' '.join(cmd)}\n# exit={proc.returncode}\n{out}")
    except OSError:
        evidence_rel = None

    # 存量棘轮（与 QUAL-002 同一原则）：仓库里已有的 cppcheck 问题冻结在基线数量上，
    # 只允许下降不允许增加；否则"接手任何遗留文件都会让门禁变红"，规则必然被绕过。
    baseline_rel = str(ctx.spec.meta.get("baseline_path", "gate/baseline.json"))
    baseline, _berr = read_json_baseline(ctx, baseline_rel)
    cppcheck_baseline: int | None = None
    if isinstance(baseline, dict):
        raw_cc = baseline.get("cppcheck")
        if isinstance(raw_cc, int):
            cppcheck_baseline = raw_cc
    effective_limit = max_errors if cppcheck_baseline is None else max(max_errors, cppcheck_baseline)

    if errors > effective_limit:
        for m in list(_CPPCHECK_RE.finditer(out))[:10]:
            loc = f"{_rel_of(ctx, m.group('file'))}:{m.group('line')}"
            findings.append(
                Finding(
                    rule=rule.id, status=Status.BLOCK, subject=loc, line=_int(m.group("line")),
                    message=f"cppcheck[{m.group('sev')}]: {m.group('msg').strip()[:220]}",
                    observed=errors, threshold=effective_limit, level="error", evidence=evidence_rel,
                    hint="按 cppcheck 提示修复；确认不是误报后可加 inline suppression 并说明理由。",
                )
            )
        findings.append(
            Finding(
                rule=rule.id, status=Status.BLOCK, subject="cppcheck",
                message=f"cppcheck 问题数 {errors} > {effective_limit}（分布 {severity_counts}）",
                observed=errors, threshold=effective_limit, level="error", evidence=evidence_rel,
            )
        )
    elif cppcheck_baseline is not None and errors > max_errors:
        findings.append(
            Finding(
                rule=rule.id, status=Status.WARN, subject="cppcheck",
                message=(
                    f"cppcheck 命中 {errors} 个存量问题（基线 {cppcheck_baseline}，阈值 {max_errors}）："
                    f"已冻结，不允许增加（分布 {severity_counts}）"
                ),
                observed=errors, threshold=effective_limit, level="warning", evidence=evidence_rel,
                hint="逐步清零后收紧 gate/baseline.json 的 cppcheck 段（规则文件变更需 CI-001 审批）。",
            )
        )
    else:
        findings.append(
            Finding(
                rule=rule.id, status=Status.PASS, subject="cppcheck",
                message=f"cppcheck 无问题（分布 {severity_counts or '{}'}，扫描 {len(targets)} 个文件）",
                observed=errors, threshold=effective_limit, level="info", evidence=evidence_rel,
            )
        )
    return findings


def _extract_include_dirs(cc_text: str) -> list[str]:
    """从 compile_commands.json 文本里抽出仓库内相对 include 目录（用于一致性核对）。"""
    dirs: set[str] = set()
    for m in re.finditer(r'"-I([^"]+)"|"-I",\s*"([^"]+)"|-I\s*([^\s"\\]+)', cc_text):
        raw = next((g for g in m.groups() if g), "")
        raw = raw.replace("\\\\", "\\").replace("\\", "/")
        if not raw or raw.startswith(("/usr", "/opt", "C:", "c:")):
            continue
        if raw.startswith("../"):
            dirs.add(raw[3:])
        elif not raw.startswith("/"):
            dirs.add(raw)
    return sorted(dirs)


if sys.platform.startswith("win"):  # pragma: no cover - 仅用于提示语措辞
    _PLATFORM_HINT = "Windows：MSYS2/LLVM 安装后加入 PATH"
else:
    _PLATFORM_HINT = "WSL：apt install g++ cppcheck"

__all__ = ["lint", "complexity", "problems_panel"]
