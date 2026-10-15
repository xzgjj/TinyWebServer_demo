"""E 族（测试部分）：TEST-001 受保护回归测试 / TEST-002 新增能力必须带测试。

TEST-001 是"改测试让失败消失"的拦截器：
  * test/** 与 benchmark/src/** 的内容 sha256 与基线不一致（含被删除）-> MANUAL（转人工），
    并明确写出需要非作者审批的例外申请命令；
  * 用例数/断言数下降 -> BLOCK。

本模块同时提供基线生成能力（`build_baseline` / `build_and_write_baseline`），
它是 SPEC 授权写盘的第二个位置（另一个是 diff_budget 的 evidence）。
"""
from __future__ import annotations

import json
import re
import time
from datetime import datetime, timezone
from pathlib import Path
from typing import Any

from ..context import Context, match_any
from ..model import Finding, Status
from ..spec import Rule
from . import (
    count_assertions,
    count_test_cases,
    glob_root,
    read_json_baseline,
    write_json_atomic,
)

CHECKS = ("protected_tests", "test_coverage")

BASELINE_GENERATOR = "scripts/gate/checks/check_tests.py::build_baseline"


# --------------------------------------------------------------------------- #
# 基线生成（可复用；CLI / selftest 通过 --update-baseline 触发）
# --------------------------------------------------------------------------- #
def build_baseline(ctx: Context) -> dict[str, Any]:
    """为受保护文件生成基线：sha256 / test_cases / assertions / lines。"""
    rule = ctx.rule("TEST-001")
    globs: list[str] = list(rule.get("protected_globs") or [])
    files: list[str] = []
    for pattern in globs:
        files.extend(glob_root(ctx.root, pattern))
    files = sorted(set(files))

    entries: dict[str, dict[str, Any]] = {}
    for rel in files:
        text = ctx.read(rel)
        digest = ctx.sha256(rel)
        if text is None or digest is None:
            continue
        entries[rel] = {
            "sha256": digest,
            "test_cases": count_test_cases(text),
            "assertions": count_assertions(text),
            "lines": len(text.splitlines()),
        }
    return {
        "generated_at": _now_iso(),
        "generator": BASELINE_GENERATOR,
        "protected_globs": globs,
        "file_count": len(entries),
        "files": entries,
    }


def build_and_write_baseline(
    root: str | Path | None = None,
    baseline_rel: str | None = None,
) -> Path:
    """生成并写入 gate/protected_tests.json，返回写入路径。

    只读纪律的例外点：仅在显式调用（`--update-baseline`）时写盘。
    """
    from ..context import build_context
    from ..model import new_run_id
    from ..spec import Spec

    repo_root = Path(root or _default_root()).resolve()
    spec = Spec.load(repo_root / "gate" / "spec.toml")
    ctx = build_context(repo_root, spec, None, new_run_id("baseline", 0))
    data = build_baseline(ctx)
    rel = baseline_rel or str(ctx.rule("TEST-001").get("baseline_file", "gate/protected_tests.json"))
    target = repo_root / rel
    write_json_atomic(target, data)
    return target


def _default_root() -> Path:
    # scripts/gate/checks/check_tests.py -> 仓库根
    return Path(__file__).resolve().parents[3]


def _now_iso() -> str:
    return datetime.now(timezone.utc).astimezone().strftime("%Y-%m-%dT%H:%M:%S%z")


# --------------------------------------------------------------------------- #
# TEST-001 受保护回归测试
# --------------------------------------------------------------------------- #
def protected_tests(ctx: Context, rule: Rule) -> list[Finding]:
    """受保护测试文件的哈希/用例数/断言数变化必须被看见。"""
    findings: list[Finding] = []
    baseline_rel = str(rule.get("baseline_file", "gate/protected_tests.json"))
    globs: list[str] = list(rule.get("protected_globs") or [])
    block_on_hash = bool(rule.get("block_on_hash_change", True))
    block_on_case = bool(rule.get("block_on_case_decrease", True))

    current_files: list[str] = []
    for pattern in globs:
        current_files.extend(glob_root(ctx.root, pattern))
    current_files = sorted(set(current_files))

    baseline, err = read_json_baseline(ctx, baseline_rel)
    if err is not None:
        findings.append(err)
        return findings
    if baseline is None:
        if _update_requested(ctx):
            data = build_baseline(ctx)
            write_json_atomic(ctx.root / baseline_rel, data)
            return [
                Finding(
                    rule=rule.id, status=Status.PASS, subject=baseline_rel,
                    message=(
                        f"基线文件不存在，已按 --update-baseline 生成："
                        f"{data['file_count']} 个受保护文件"
                    ),
                    observed=data["file_count"], threshold=">=1", level="info",
                    hint="把基线文件纳入版本控制，否则受保护测试形同虚设。",
                )
            ]
        return [
            Finding(
                rule=rule.id, status=Status.WARN, subject=baseline_rel,
                message=(
                    f"受保护测试基线缺失（{baseline_rel}）：无法判断 test/** 是否被改动，"
                    "TEST-001 本次为盲区"
                ),
                observed=None, threshold="存在基线文件",
                hint=(
                    "生成基线（仓库根执行）: "
                    "python -X utf8 -c \"import sys; sys.path.insert(0, 'scripts'); "
                    "from gate.checks.check_tests import build_and_write_baseline; "
                    "build_and_write_baseline()\""
                ),
            )
        ]

    entries: dict[str, Any] = baseline.get("files") if isinstance(baseline.get("files"), dict) else {}
    if not entries:
        findings.append(
            Finding(
                rule=rule.id, status=Status.WARN, subject=baseline_rel,
                message="基线文件没有 files 段（旧格式或损坏），本规则为盲区",
                observed=sorted(baseline)[:8], threshold="含 files 的对象",
                hint="重新生成基线（--update-baseline）。",
            )
        )
        return findings

    # ① 内容变化 / 删除
    changed: list[str] = []
    deleted: list[str] = []
    for rel, meta in sorted(entries.items()):
        expect = str((meta or {}).get("sha256", "")) if isinstance(meta, dict) else ""
        actual = ctx.sha256(rel)
        if actual is None:
            deleted.append(rel)
        elif actual != expect:
            changed.append(rel)

    new_files = [p for p in current_files if p not in entries]
    for rel in changed + deleted:
        kind = "已删除" if rel in deleted else "内容被修改"
        status = Status.MANUAL if block_on_hash else Status.WARN
        findings.append(
            Finding(
                rule=rule.id, status=status, subject=rel,
                message=(
                    f"受保护测试 {kind}（sha256 与基线不一致）"
                    + ("，需要非作者审批" if block_on_hash else "")
                ),
                observed="sha256 changed" if rel in changed else "missing",
                threshold=str((entries.get(rel) or {}).get("sha256", ""))[:12] + "…",
                level="error" if block_on_hash else "warning",
                evidence=baseline_rel,
                hint=(
                    "需要非作者审批：`python scripts/gate/cli.py exception request --rule TEST-001`"
                    "（由 meta.rule_approvers 中的人批准后重跑门禁）"
                ),
            )
        )
    if new_files:
        findings.append(
            Finding(
                rule=rule.id, status=Status.WARN, subject="(新增测试文件)",
                message=f"基线里没有这些受保护文件，属新增：{new_files[:10]}",
                observed=len(new_files), threshold=0, level="warning",
                hint="新增测试是好事情，但需要重生成基线才能继续保护它：带 --update-baseline 重跑。",
            )
        )

    # ② 用例数 / 断言数下降
    for rel, meta in sorted(entries.items()):
        if not isinstance(meta, dict):
            continue
        text = ctx.read(rel)
        if text is None:
            continue
        for key, counter, label in (
            ("test_cases", count_test_cases, "用例数"),
            ("assertions", count_assertions, "断言数"),
        ):
            old = _as_int(meta.get(key))
            if old is None:
                continue
            new = counter(text)
            if new < old:
                findings.append(
                    Finding(
                        rule=rule.id,
                        status=Status.BLOCK if block_on_case else Status.WARN,
                        subject=rel,
                        message=f"{label}下降：{new} < 基线 {old}（禁止用删测试让失败消失）",
                        observed=new, threshold=old,
                        level="error" if block_on_case else "warning",
                        evidence=baseline_rel,
                        hint="恢复被删除的用例/断言；确需变更必须走例外审批（TEST-001 转人工）。",
                    )
                )

    if not changed and not deleted:
        findings.append(
            Finding(
                rule=rule.id, status=Status.PASS, subject=baseline_rel,
                message=(
                    f"受保护文件 {len(entries)} 个全部与基线一致"
                    f"（当前受保护集合 {len(current_files)} 个）"
                ),
                observed=len(current_files), threshold=len(entries), level="info",
            )
        )
    return findings


def _update_requested(ctx: Context) -> bool:
    for key in ("update_baseline", "update-baseline"):
        if ctx.options.get(key):
            return True
    return False


def _as_int(value: Any) -> int | None:
    try:
        return int(value)
    except (TypeError, ValueError):
        return None


# --------------------------------------------------------------------------- #
# TEST-002 新增能力必须带测试
# --------------------------------------------------------------------------- #
_REGISTERED_TEST_RE = re.compile(r"test_[A-Za-z0-9_]+")
_CMAKE_VAR_FILE_RE = re.compile(r"\$\{[^}]*\}\s*/\s*test\s*/\s*\$\{?\w*\}?\.cpp")


def test_coverage(ctx: Context, rule: Rule) -> list[Finding]:
    """新增源文件必须有对应 test_*.cpp 且被 CMake 注册；smoke 必须可用。"""
    findings: list[Finding] = []
    src_globs: list[str] = list(rule.get("src_globs") or [])
    test_dir = str(rule.get("test_dir", "test"))
    cmake_file = str(rule.get("cmake_file", "CMakeLists.txt"))
    require_smoke = bool(rule.get("require_ctest_smoke", False))
    smoke_label = str(rule.get("smoke_label", "smoke"))

    cmake_text = ctx.read(cmake_file)
    registered = set(_REGISTERED_TEST_RE.findall(cmake_text or ""))
    cmake_has_var_loop = bool(_CMAKE_VAR_FILE_RE.search(cmake_text or ""))

    new_sources: list[str] = []
    for rel, code in ctx.git.status_map.items():
        if code not in {"??", "A", "AM"}:
            continue
        if ctx.excluded(rel):
            continue
        if not match_any(rel, src_globs):
            continue
        if not (ctx.root / rel).is_file():
            continue
        new_sources.append(rel)
    for rel in ctx.affected_matching(src_globs):
        if ctx.git.status_map.get(rel) in {"??", "A", "AM"} and rel not in new_sources:
            new_sources.append(rel)

    if new_sources:
        for rel in sorted(new_sources):
            candidates = _test_candidates(test_dir, rel)
            existing = [c for c in candidates if ctx.exists(c)]
            if not existing:
                findings.append(
                    Finding(
                        rule=rule.id, status=Status.BLOCK, subject=rel,
                        message=f"新增源文件 {rel} 没有对应测试文件（期望 {' 或 '.join(candidates)}）",
                        observed={"source": rel, "test_candidates": candidates},
                        threshold=candidates[0], level="error",
                        hint=f"新增 {candidates[0]}，或在契约中说明该文件为何不需要测试。",
                    )
                )
                continue
            test_rel = existing[0]
            stem = Path(test_rel).stem
            if stem not in registered:
                findings.append(
                    Finding(
                        rule=rule.id, status=Status.BLOCK, subject=test_rel,
                        message=f"测试文件 {test_rel} 未被 {cmake_file} 注册（ctest 永远不会跑它）",
                        observed=stem, threshold=f"{cmake_file} 中的测试目标名", level="error",
                        hint=f"把 {stem} 加入 {cmake_file} 的测试目标列表（INTEGRATION_TESTS 或 ALL_TEST_TARGETS）。",
                    )
                )
            else:
                findings.append(
                    Finding(
                        rule=rule.id, status=Status.PASS, subject=rel,
                        message=f"新增源文件 {rel} 有对应测试 {test_rel}，且已在 {cmake_file} 注册",
                        observed=test_rel, threshold=candidates[0], level="info",
                    )
                )
    else:
        findings.append(
            Finding(
                rule=rule.id, status=Status.PASS, subject="(新增源文件)",
                message=f"本次没有新增 src 文件（src_globs={src_globs}），无需配套测试",
                observed=0, threshold=0, level="info",
            )
        )
    if cmake_has_var_loop:
        findings.append(
            Finding(
                rule=rule.id, status=Status.PASS, subject=cmake_file,
                message="CMake 使用 test_${test_name}.cpp 型变量循环注册测试（已计入注册判定）",
                observed=True, threshold=True, level="info",
            )
        )

    if require_smoke:
        findings.append(_smoke_finding(ctx, rule, smoke_label))
    return findings


def _test_candidates(test_dir: str, src_rel: str) -> list[str]:
    stem = Path(src_rel).stem
    base = test_dir.strip("/")
    return [f"{base}/test_{stem}.cpp", f"{base}/{stem}_test.cpp", f"{base}/{stem}.cpp"]


def _smoke_finding(ctx: Context, rule: Rule, smoke_label: str) -> Finding:
    """CTest 冒烟集可用性：不可用 -> WARN + 明确命令（不得假装通过）。"""
    ctest_file = ctx.root / "build" / "CTestTestfile.cmake"
    last_log = ctx.root / "build" / "Testing" / "Temporary" / "LastTest.log"
    if not ctest_file.is_file():
        return Finding(
            rule=rule.id, status=Status.WARN, subject="build/CTestTestfile.cmake",
            message="未找到 CTest 配置，冒烟集无法验证（require_ctest_smoke=true）",
            observed=False, threshold=True, level="warning",
            hint="配置并构建: cmake -S . -B build -DBUILD_TESTING=ON && cmake --build build",
        )
    if not last_log.is_file():
        return Finding(
            rule=rule.id, status=Status.WARN, subject="build/Testing/Temporary/LastTest.log",
            message="没有 ctest 运行记录，冒烟集未验证",
            observed=False, threshold=True, level="warning",
            hint=f"运行冒烟集: ctest --test-dir build -L {smoke_label} --output-on-failure",
        )
    try:
        mtime = last_log.stat().st_mtime
    except OSError as exc:
        return Finding(
            rule=rule.id, status=Status.WARN, subject="build/Testing/Temporary/LastTest.log",
            message=f"ctest 记录无法读取: {exc}",
            observed=type(exc).__name__, threshold="可读文件", level="warning",
        )
    passed, failed = _parse_last_test_log(last_log)
    age_h = (time.time() - mtime) / 3600.0
    if failed:
        return Finding(
            rule=rule.id, status=Status.BLOCK, subject="build/Testing/Temporary/LastTest.log",
            message=f"最近一次 ctest 有失败用例：passed={passed}, failed={failed}（{age_h:.1f} 小时前）",
            observed=failed, threshold=0, level="error",
            hint=f"先修失败用例: ctest --test-dir build -L {smoke_label} --output-on-failure",
        )
    stale_h = _stale_hours(ctx)
    if age_h > stale_h:
        return Finding(
            rule=rule.id, status=Status.WARN, subject="build/Testing/Temporary/LastTest.log",
            message=(
                f"最近一次 ctest 记录已过期 {age_h:.1f} 小时，晚于代码最后修改时间"
                f"（阈值 {stale_h:.1f} 小时），冒烟集未在本次改动后验证过"
            ),
            observed=round(age_h, 1), threshold=stale_h, level="warning",
            hint=f"重跑冒烟集: ctest --test-dir build -L {smoke_label} --output-on-failure",
        )
    return Finding(
        rule=rule.id, status=Status.PASS, subject="build/Testing/Temporary/LastTest.log",
        message=f"ctest 记录可用：passed={passed}, failed={failed}（{age_h:.1f} 小时前）",
        observed=passed, threshold="failed=0", level="info",
    )


def _stale_hours(ctx: Context) -> float:
    """新鲜度阈值：优先取 max(1h, 最近改动文件距今的小时数)，保证"改动后必须重跑"。"""
    stale_h = 1.0
    for rel in ctx.affected()[:50]:
        fp = ctx.root / rel
        try:
            age = (time.time() - fp.stat().st_mtime) / 3600.0
        except OSError:
            continue
        stale_h = max(stale_h, age)
    return stale_h


_END_RE = re.compile(r"(?m)^\s*(?P<pct>\d+)%\s+tests passed,\s*(?P<failed>\d+)\s+tests failed out of\s*(?P<total>\d+)")


def _parse_last_test_log(path: Path) -> tuple[int, int]:
    """解析 ctest 的 LastTest.log，返回 (passed, failed)。只读文件尾部，避免大文件拖慢门禁。"""
    try:
        size = path.stat().st_size
        with path.open("r", encoding="utf-8", errors="replace") as fh:
            if size > 2_000_000:
                fh.seek(size - 2_000_000)
                fh.readline()
            text = fh.read()
    except OSError:
        return 0, 0
    lines = text.splitlines()
    failed = 0
    passed = 0
    for i, line in enumerate(lines):
        if not line.strip():
            continue
        # ctest 的逐用例段落：以 `N/M Test #K: name .....` 开头
        m = re.match(r"^\s*\d+/\d+\s+Test\s+#\d+:\s+(?P<name>\S+)", line)
        if not m:
            continue
        tail = "\n".join(lines[i: i + 40])
        if re.search(r"\*\*\*Failed|\*\*\*Exception|\*\*\*Timeout|Unable to find executable", tail):
            failed += 1
        elif re.search(r"Passed\.|^\s*\d+/\d+\s+Test\s+#\d+:.*\bPassed\b", tail, re.MULTILINE):
            passed += 1
    if passed == 0 and failed == 0:
        m_end = _END_RE.search(text)
        if m_end:
            total = int(m_end.group("total"))
            failed = int(m_end.group("failed"))
            passed = total - failed
    return passed, failed
