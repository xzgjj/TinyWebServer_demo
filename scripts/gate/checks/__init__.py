"""门禁判定器注册表（checks 包）。

约定（与 scripts/gate/spec.py 的 KNOWN_CHECKS 双向绑定）：
  * 每个 check_*.py 暴露 `run(ctx, rule) -> list[Finding]`；
  * REGISTRY 由 `load_registry()` 自动发现，不手写列表——手写列表会漏掉新模块，
    而 `Spec.check_registry_consistency()` 会立刻把这种不一致报成 SpecError；
  * 这里同时放"跨 check 共用的小工具"（计数口径、基线读写、证据路径），
    保证同一口径只有一处实现，避免各规则各算一套。

只读纪律：本模块只做发现与读取；唯一写盘的是 `write_json_atomic()`，
调用方仅限 SPEC 明确授权写盘的两处（diff_budget 的证据文件、protected_tests 的基线）。
"""
from __future__ import annotations

import importlib
import json
import os
import re
import sys
import tempfile
from pathlib import Path
from typing import Any, Callable

from ..model import Finding, Status
from ..spec import Rule

try:  # 包式导入（python -m scripts.gate.selftest / 由 runner 导入）
    from ..context import Context
except ImportError:  # pragma: no cover - 直接以脚本方式运行时的兜底
    sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
    from gate.context import Context  # type: ignore[no-redef]

__all__ = ["REGISTRY", "load_registry", "RunFn"]


RunFn = Callable[[Context, Rule], list[Finding]]


# --------------------------------------------------------------------------- #
# 注册表自动发现
# --------------------------------------------------------------------------- #
def load_registry() -> dict[str, RunFn]:
    """导入同目录下所有 check_*.py，返回 {check 名: run 函数}。

    每个 check_*.py 用模块级 `CHECKS = ("doc_budget", ...)` 显式声明自己提供的
    判定器（check 名必须与 gate/spec.toml 里的 `check = "..."` 逐字一致）：
      * 显式声明的好处是可双向校验——多写一个名字或漏写一个名字，都会在
        `Spec.check_registry_consistency()` 处暴露为 SpecError，而不是静默通过；
      * 不用 `dir(module)` 猜测，避免把编译好的正则、辅助函数误当判定器注册。
    """
    registry: dict[str, RunFn] = {}
    here = Path(__file__).resolve().parent
    for path in sorted(here.glob("check_*.py")):
        module = importlib.import_module(f"{__name__}.{path.stem}")
        declared = getattr(module, "CHECKS", None)
        if not isinstance(declared, (tuple, list)) or not declared:
            raise RuntimeError(
                f"{path.name} 未声明 CHECKS 元组（每个 check 模块必须显式声明提供的判定器）"
            )
        for name in declared:
            fn = getattr(module, str(name), None)
            if not callable(fn):
                raise RuntimeError(f"{path.name} 声明了 CHECKS 项 {name!r}，但模块内没有可调用对象")
            if name in registry:
                raise RuntimeError(f"check {name!r} 被重复注册（{path.name}）")
            registry[str(name)] = fn
    return registry


REGISTRY: dict[str, RunFn] = {}
# 注意：REGISTRY 在文件末尾才填充。
# 原因：各 check_*.py 需要 `from . import 工具函数`，若在文件中部提前调用
# load_registry()，子模块会在 helpers 定义完成前被导入，触发循环导入。
# 这里的顺序是刻意的：先定义全部共用工具 -> 再自动发现并注册判定器。


# --------------------------------------------------------------------------- #
# 计数口径
# --------------------------------------------------------------------------- #
_WS_RE = re.compile(r"\s")


def non_whitespace_chars(text: str) -> int:
    """DOC 系列统一字数口径：非空白字符数（中英一致）。"""
    return len(_WS_RE.sub("", text))


def count_test_cases(text: str) -> int:
    """统计 TEST()/TEST_F()/TEST_P()/TYPED_TEST() 用例数。"""
    return len(re.findall(r"(?m)^[ \t]*(?:TEST|TEST_F|TEST_P|TYPED_TEST)\s*\(", text))


def count_assertions(text: str) -> int:
    """统计 GTest 断言数（EXPECT_*/ASSERT_*/FAIL/SUCCEED/ADD_FAILURE）。"""
    return len(
        re.findall(r"\b(?:EXPECT|ASSERT)_[A-Z_]+\s*\(|\b(?:FAIL|SUCCEED|ADD_FAILURE)\s*\(", text)
    )


# --------------------------------------------------------------------------- #
# 路径口径
# --------------------------------------------------------------------------- #
def safe_join(root: Path, rel_path: str) -> Path | None:
    """把仓库相对路径解析为绝对路径；越界（../ 逃逸）返回 None。"""
    if not rel_path:
        return None
    try:
        root_r = root.resolve()
        candidate = (root_r / rel_path.replace("\\", "/")).resolve()
    except OSError:
        return None
    if candidate != root_r and root_r not in candidate.parents:
        return None
    return candidate


def glob_root(root: Path, pattern: str) -> list[str]:
    """按仓库相对 POSIX 路径 glob（支持 `**`），返回排序后的相对路径。"""
    pat = pattern.replace("\\", "/")
    if not pat or ".." in pat.split("/"):
        return []
    if pat.startswith("/"):
        return []
    return sorted(
        p.relative_to(root).as_posix() for p in root.glob(pat) if p.is_file()
    )


# --------------------------------------------------------------------------- #
# 基线读写（gate/baseline.json 与 gate/protected_tests.json 共用）
# --------------------------------------------------------------------------- #
def baseline_rel_path(ctx: Context, override: str | None = None) -> str:
    """基线文件相对路径：调用方显式指定优先，否则取 [meta].baseline_path。"""
    if override:
        return override.replace("\\", "/")
    return str(ctx.spec.meta.get("baseline_path", "gate/baseline.json")).replace("\\", "/")


def read_json_baseline(ctx: Context, rel_path: str) -> tuple[dict[str, Any] | None, Finding | None]:
    """读 JSON 基线。返回 (数据, 错误 finding)。

    - 文件不存在 -> (None, None)：调用方决定是 WARN 还是按"无基线"处理；
    - 解析失败/越界 -> (None, Finding)：绝不静默当成"通过"。
    """
    path = safe_join(ctx.root, rel_path)
    if path is None:
        return None, Finding(
            rule="-", status=Status.ERROR, subject=rel_path,
            message=f"基线路径越出仓库根目录，拒绝读取: {rel_path}",
            observed=rel_path, threshold="仓库内相对路径",
        )
    if not path.is_file():
        return None, None
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeDecodeError, json.JSONDecodeError) as exc:
        return None, Finding(
            rule="-", status=Status.ERROR, subject=rel_path,
            message=f"基线文件无法解析（请修复或重新生成）: {exc}",
            observed=type(exc).__name__, threshold="合法 JSON",
            hint=f"删除后重新生成: {rel_path}",
        )
    if not isinstance(data, dict):
        return None, Finding(
            rule="-", status=Status.ERROR, subject=rel_path,
            message="基线文件顶层必须是 JSON 对象",
            observed=type(data).__name__, threshold="object",
        )
    return data, None


def write_json_atomic(path: Path, data: dict[str, Any]) -> None:
    """原子写 JSON（仅 SPEC 授权的两处调用）。"""
    path.parent.mkdir(parents=True, exist_ok=True)
    fd, tmp_name = tempfile.mkstemp(dir=str(path.parent), prefix=path.name, suffix=".tmp")
    try:
        with os.fdopen(fd, "w", encoding="utf-8", newline="\n") as fh:
            json.dump(data, fh, ensure_ascii=False, indent=2, sort_keys=True)
            fh.write("\n")
        os.replace(tmp_name, path)
    except BaseException:
        try:
            os.unlink(tmp_name)
        except OSError:
            pass
        raise


def write_text_atomic(path: Path, text: str) -> None:
    """原子写文本（证据文件用）。"""
    path.parent.mkdir(parents=True, exist_ok=True)
    fd, tmp_name = tempfile.mkstemp(dir=str(path.parent), prefix=path.name, suffix=".tmp")
    try:
        with os.fdopen(fd, "w", encoding="utf-8", newline="\n") as fh:
            fh.write(text)
        os.replace(tmp_name, path)
    except BaseException:
        try:
            os.unlink(tmp_name)
        except OSError:
            pass
        raise


# --------------------------------------------------------------------------- #
# 证据目录
# --------------------------------------------------------------------------- #
def report_dir(ctx: Context) -> Path:
    return ctx.root / str(ctx.spec.meta.get("report_dir", "local/gate"))


def evidence_path(ctx: Context, rel_name: str, *, suffix: str = ".txt") -> tuple[Path, str]:
    """返回 (绝对路径, 仓库相对 POSIX 路径)：local/gate/<run_id>/evidence/<name>。"""
    name = rel_name if rel_name.endswith(suffix) else f"{rel_name}{suffix}"
    abs_path = report_dir(ctx) / ctx.run_id / "evidence" / name
    return abs_path, ctx.rel(abs_path)


def code_source_set(ctx: Context) -> list[str]:
    """QUAL-001 兜底源集：无改动代码文件时，检查整个核心源集。"""
    exts = tuple(ctx.spec.code_exts)
    out: list[str] = []
    for pattern in ("src/**/*", "include/**/*", "reactor/**/*"):
        for rel in glob_root(ctx.root, pattern):
            if rel.endswith(exts):
                out.append(rel)
    return sorted(set(out))


# --------------------------------------------------------------------------- #
# 注册（必须放在文件末尾，见上方注释）
# --------------------------------------------------------------------------- #
REGISTRY.update(load_registry())
