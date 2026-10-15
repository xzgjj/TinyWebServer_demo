"""门禁事实采集层：一次采集，多条规则只读共享。

为什么要单独一层：
  * 规则只做判定，不做 IO；保证同一份事实只测一次（可缓存、可复现、可复核）。
  * 采集口径固定写在这里（worktree 相对 HEAD / CI 的 A..B 区间），避免各规则各算一套。
"""
from __future__ import annotations

import dataclasses
import hashlib
import os
import re
import subprocess
import time
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Iterable

from .spec import Rule, Spec
from .task import TaskContract


# --------------------------------------------------------------------------- #
# glob（支持 ** 语义，POSIX 路径）
# --------------------------------------------------------------------------- #
def _glob_to_regex(pattern: str) -> re.Pattern[str]:
    p = pattern.replace("\\", "/").strip()
    i = 0
    out = ["^"]
    while i < len(p):
        c = p[i]
        if p.startswith("**/", i):
            out.append("(?:.*/)?")
            i += 3
        elif p.startswith("**", i):
            out.append(".*")
            i += 2
        elif c == "*":
            out.append("[^/]*")
            i += 1
        elif c == "?":
            out.append("[^/]")
            i += 1
        else:
            out.append(re.escape(c))
            i += 1
    out.append("$")
    return re.compile("".join(out))


_GLOB_CACHE: dict[str, re.Pattern[str]] = {}


def glob_match(path: str, pattern: str) -> bool:
    """path 与 pattern 均为仓库相对 POSIX 路径。"""
    rx = _GLOB_CACHE.get(pattern)
    if rx is None:
        rx = _GLOB_CACHE[pattern] = _glob_to_regex(pattern)
    return bool(rx.match(path.replace("\\", "/")))


def match_any(path: str, patterns: Iterable[str]) -> bool:
    return any(glob_match(path, pat) for pat in patterns)


# --------------------------------------------------------------------------- #
# git 事实
# --------------------------------------------------------------------------- #
@dataclass
class GitFacts:
    mode: str = "worktree"                  # worktree | range
    ref_range: str = ""                     # 仅 range 模式: "A..B"
    available: bool = True                  # git 是否真的读到了事实（collect_git 会显式覆盖）
    commit: str = ""
    commit_subject: str = ""
    branch: str = ""
    author_name: str = ""
    author_email: str = ""
    #: 被审查改动集的**全部**提交作者（range 模式取自 git log，worktree 模式取 git config）
    authors: list[str] = field(default_factory=list)
    changed: list[str] = field(default_factory=list)          # 已改动（含 untracked）
    numstat: dict[str, tuple[int, int]] = field(default_factory=dict)  # path -> (add, del)
    status_map: dict[str, str] = field(default_factory=dict)  # path -> porcelain 状态码
    untracked: list[str] = field(default_factory=list)

    def lines_changed(self, paths: Iterable[str] | None = None) -> int:
        keys = list(self.numstat) if paths is None else [p for p in paths if p in self.numstat]
        return sum(a + d for (a, d) in (self.numstat[k] for k in keys))

    def added(self) -> int:
        return sum(a for a, _ in self.numstat.values())

    def deleted(self) -> int:
        return sum(d for _, d in self.numstat.values())


def _run(cmd: list[str], cwd: Path, timeout: int = 120) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        cmd,
        cwd=str(cwd),
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="replace",
        timeout=timeout,
    )


def collect_git(root: Path, ref_range: str = "") -> GitFacts:
    facts = GitFacts(mode="range" if ref_range else "worktree", ref_range=ref_range)

    def out(*args: str) -> str:
        try:
            r = _run(["git", *args], root)
            return r.stdout if r.returncode == 0 else ""
        except (OSError, subprocess.SubprocessError):
            return ""

    def out_rc(*args: str) -> tuple[int, str]:
        try:
            r = _run(["git", *args], root)
            return r.returncode, (r.stdout or "")
        except (OSError, subprocess.SubprocessError):
            return 128, ""

    facts.commit = out("rev-parse", "--short", "HEAD").strip()
    facts.available = bool(facts.commit)
    facts.commit_subject = out("log", "-1", "--pretty=%s").strip()
    facts.branch = out("rev-parse", "--abbrev-ref", "HEAD").strip()
    facts.author_name = out("config", "user.name").strip()
    facts.author_email = out("config", "user.email").strip()
    facts.authors = [facts.author_name] if facts.author_name else []

    # range 模式下，"变更作者"必须取自**提交元数据**而不是 git config：
    #   * config 只是"当前这台机器的默认身份"，任何人改一下就能冒充别人（可被绕过）；
    #   * 被审查的是一段提交，其作者可能不止一个（人 + 代理），必须都看到。
    if ref_range:
        raw_authors = out("log", "--format=%an <%ae>", ref_range)
        uniq = [a.strip() for a in dict.fromkeys(raw_authors.splitlines()) if a.strip()]
        if uniq:
            facts.authors = uniq
            newest = out("log", "-1", "--format=%an", ref_range).strip()
            if newest:
                facts.author_name = newest

    if ref_range:
        ns_rc, ns = out_rc("diff", "--numstat", ref_range)
        names_rc, names = out_rc("diff", "--name-status", ref_range)
        # 关键：range 模式下如果 git diff 本身失败（例如 CI 首次 push 的 before 是全 0 SHA、
        # 或 base 不存在），旧实现会静默返回空串 → "改动集为空" → 范围/预算类规则全绿。
        # 那是"什么都看不见却通过"。这里显式标记为不可信，由 SCOPE 类规则判 ERROR。
        if ns_rc != 0 or names_rc != 0:
            facts.available = False
    else:
        ns = out("diff", "--numstat", "HEAD")
        # -uall：把未跟踪目录展开成具体文件（默认会折叠成 "?? dir/"，导致新增目录不可见）
        names = out("status", "--porcelain", "-uall")

    for line in ns.splitlines():
        parts = line.split("\t")
        if len(parts) < 3:
            continue
        a, d, path = parts[0], parts[1], "\t".join(parts[2:])
        try:
            added = int(a) if a != "-" else 0
            deleted = int(d) if d != "-" else 0
        except ValueError:
            added = deleted = 0
        path = path.replace("\\", "/")
        facts.numstat[path] = (added, deleted)
        if path not in facts.changed:
            facts.changed.append(path)

    for line in names.splitlines():
        if not line.strip():
            continue
        if ref_range:
            parts = line.split("\t")
            if len(parts) >= 2:
                code, path = parts[0], parts[-1]
            else:
                continue
        else:
            code, path = line[:2].strip(), line[3:]
        path = path.strip().replace("\\", "/")
        if " -> " in path:  # rename
            path = path.split(" -> ")[-1]
        facts.status_map[path] = code or "?"
        if path not in facts.changed:
            facts.changed.append(path)
        if code == "??":
            facts.untracked.append(path)

    # untracked 文件没有 numstat，按实际行数计入 diff 预算（否则"新增大文件"可绕过预算）
    for path in facts.untracked:
        fp = root / path
        if fp.is_file():
            try:
                text = fp.read_text(encoding="utf-8", errors="replace")
                facts.numstat[path] = (len(text.splitlines()), 0)
            except OSError:
                facts.numstat[path] = (0, 0)

    return facts


# --------------------------------------------------------------------------- #
# 上下文
# --------------------------------------------------------------------------- #
@dataclass
class Context:
    root: Path
    spec: Spec
    task: TaskContract | None
    run_id: str
    git: GitFacts
    options: dict[str, Any] = field(default_factory=dict)
    cache: dict[str, Any] = field(default_factory=dict)
    notes: list[str] = field(default_factory=list)
    started: float = field(default_factory=time.time)

    # ---- 路径 -------------------------------------------------------------- #
    def rel(self, path: str | Path) -> str:
        p = Path(path)
        try:
            return p.resolve().relative_to(self.root.resolve()).as_posix()
        except ValueError:
            return p.as_posix()

    def read(self, rel_path: str) -> str | None:
        key = f"read::{rel_path}"
        if key in self.cache:
            return self.cache[key]
        fp = self.root / rel_path
        text: str | None
        try:
            text = fp.read_text(encoding="utf-8", errors="replace")
        except (OSError, IsADirectoryError):
            text = None
        self.cache[key] = text
        return text

    def sha256(self, rel_path: str) -> str | None:
        key = f"sha::{rel_path}"
        if key in self.cache:
            return self.cache[key]
        fp = self.root / rel_path
        try:
            h = hashlib.sha256(fp.read_bytes()).hexdigest()
        except OSError:
            h = None
        self.cache[key] = h
        return h

    def exists(self, rel_path: str) -> bool:
        return (self.root / rel_path).exists()

    # ---- 过滤 -------------------------------------------------------------- #
    def excluded(self, path: str) -> bool:
        return match_any(path, self.spec.exclude_globs)

    def affected(self) -> list[str]:
        """本次改动中真正参与判定的文件（剔除构建产物/第三方/忽略项）。"""
        if "affected" not in self.cache:
            self.cache["affected"] = [
                p for p in self.git.changed if not self.excluded(p) and (self.root / p).is_file()
            ]
        return list(self.cache["affected"])

    def affected_matching(self, patterns: Iterable[str]) -> list[str]:
        pats = list(patterns)
        return [p for p in self.affected() if match_any(p, pats)]

    def code_files(self) -> list[str]:
        exts = tuple(self.spec.code_exts)
        return [p for p in self.affected() if p.endswith(exts)]

    def is_protected(self, path: str) -> bool:
        return match_any(path, self.spec.protected_paths)

    def protected_changed(self) -> list[str]:
        return [p for p in self.git.changed if self.is_protected(p)]

    # ---- 规格 -------------------------------------------------------------- #
    def rule(self, rule_id: str) -> Rule:
        return self.spec.rule(rule_id)

    # ---- 进程 -------------------------------------------------------------- #
    def run(
        self,
        cmd: list[str],
        timeout: int = 300,
        cwd: str | Path | None = None,
        env: dict[str, str] | None = None,
    ) -> subprocess.CompletedProcess[str]:
        merged = os.environ.copy()
        if env:
            merged.update(env)
        try:
            return subprocess.run(
                cmd,
                cwd=str(cwd or self.root),
                capture_output=True,
                text=True,
                encoding="utf-8",
                errors="replace",
                timeout=timeout,
                env=merged,
            )
        except subprocess.TimeoutExpired as exc:
            return subprocess.CompletedProcess(
                cmd, 124, exc.stdout or "", f"TIMEOUT after {timeout}s: {' '.join(cmd)}"
            )
        except OSError as exc:
            return subprocess.CompletedProcess(cmd, 127, "", f"{exc}")

    def log(self, message: str) -> None:
        self.notes.append(message)

    # ---- 预算文档 ---------------------------------------------------------- #
    def doc_candidates(self) -> dict[str, str]:
        """返回 {相对路径: 文本} —— 仅包含规格中出现的、且真实存在的文档。"""
        p = self.rule("DOC-001").params
        names: list[str] = []
        names += list((p.get("budgets") or {}).keys())
        names += list(p.get("grandfathered") or [])
        for pattern in p.get("unlimited_globs") or []:
            if "*" in pattern:
                for fp in sorted(self.root.glob(pattern)):
                    if fp.is_file():
                        names.append(self.rel(fp))
            else:
                names.append(pattern)
        out: dict[str, str] = {}
        for name in dict.fromkeys(names):
            text = self.read(name)
            if text is not None:
                out[name] = text
        return out

    def to_summary(self) -> dict[str, Any]:
        return {
            "root": str(self.root),
            "run_id": self.run_id,
            "task": self.task.id if self.task else None,
            "task_state": self.task.state if self.task else None,
            "attempt": self.task.attempts if self.task else 0,
            "git": {
                "mode": self.git.mode,
                "ref_range": self.git.ref_range,
                "commit": self.git.commit,
                "branch": self.git.branch,
                "author": self.git.author_name,
                "changed_files": len(self.git.changed),
                "affected_files": len(self.affected()),
                "lines_changed": self.git.lines_changed(),
            },
            "spec_hash": self.spec.spec_hash,
            "notes": self.notes,
        }


def build_context(
    root: Path,
    spec: Spec,
    task: TaskContract | None,
    run_id: str,
    options: dict[str, Any] | None = None,
) -> Context:
    opts = dict(options or {})
    ref_range = str(opts.get("base") or (task.base_ref if task else "") or "")
    git = collect_git(root, ref_range)
    return Context(root=root, spec=spec, task=task, run_id=run_id, git=git, options=opts)


# 供 checks 复用的工具
def strip_comments_preserve_lines(text: str, exts: tuple[str, ...] = (".cpp", ".h", ".hpp")) -> str:
    """把注释替换为等长空白，保留行号与代码结构（供重复检测/复杂度分析使用）。"""
    out: list[str] = []
    i = 0
    n = len(text)
    in_line = False
    in_block = False
    in_str: str | None = None
    while i < n:
        c = text[i]
        nxt = text[i + 1] if i + 1 < n else ""
        if in_line:
            if c == "\n":
                in_line = False
                out.append(c)
            else:
                out.append(" " if c != "\t" else "\t")
            i += 1
            continue
        if in_block:
            if c == "*" and nxt == "/":
                out.append("  ")
                in_block = False
                i += 2
                continue
            out.append("\n" if c == "\n" else " ")
            i += 1
            continue
        if in_str:
            out.append(c)
            if c == "\\" and i + 1 < n:
                out.append(text[i + 1])
                i += 2
                continue
            if c == in_str:
                in_str = None
            i += 1
            continue
        if c == "/" and nxt == "/":
            in_line = True
            out.append("  ")
            i += 2
            continue
        if c == "/" and nxt == "*":
            in_block = True
            out.append("  ")
            i += 2
            continue
        if c in ('"', "'"):
            in_str = c
            out.append(c)
            i += 1
            continue
        out.append(c)
        i += 1
    return "".join(out)


def normalize_code_line(line: str) -> str:
    """归一化：去空白与常见噪声，用于重复块指纹。"""
    s = re.sub(r"\s+", "", line)
    s = re.sub(r"\b\d+\b", "0", s)
    return s
