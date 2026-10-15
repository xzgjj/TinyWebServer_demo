#!/usr/bin/env python3
"""编辑器问题面板的等价校验（VS Code Problems panel 的可执行代理）。

为什么需要它：
"项目完成时 VS Code Problems 面板 0 报错"这条验收标准，无法在无头环境里直接读面板。
但面板的报错来源是确定的：**按 compile_commands.json 的真实编译参数解析每个文件**。
因此这里复现同一件事：逐文件 `-fsyntax-only`（与编辑器同源），再可选叠加 cppcheck。

用法:
    python scripts/check_problems_panel.py [--build-dir build] [--cppcheck] [--quiet]

退出码: 0 = 0 错误；1 = 存在错误；3 = 找不到 compile_commands.json
"""
from __future__ import annotations

import argparse
import json
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


def load_entries(build_dir: Path) -> list[dict]:
    cc = build_dir / "compile_commands.json"
    if not cc.is_file():
        print(f"[错误] 找不到 {cc}：请先执行 cmake 配置（会生成编译数据库）", file=sys.stderr)
        sys.exit(3)
    data = json.loads(cc.read_text(encoding="utf-8", errors="replace"))
    if not isinstance(data, list) or not data:
        print(f"[错误] {cc} 内容为空或格式不符", file=sys.stderr)
        sys.exit(3)
    return [e for e in data if isinstance(e, dict)]


def syntax_args(entry: dict) -> list[str]:
    """把编译命令改写为 -fsyntax-only（去掉 -o/-c 与目标文件）。"""
    tokens = entry.get("arguments")
    if not isinstance(tokens, list):
        tokens = str(entry.get("command", "")).split()
    out: list[str] = []
    skip_next = False
    for tok in tokens:
        if skip_next:
            skip_next = False
            continue
        if tok in ("-o", "-c", "/c", "/Fo"):
            skip_next = tok in ("-o", "/Fo")
            continue
        if tok.endswith((".o", ".obj")):
            continue
        if tok.endswith((".cpp", ".cc", ".cxx", ".c")):
            continue
        out.append(tok)
    return out


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="问题面板等价校验")
    parser.add_argument("--build-dir", default="build")
    parser.add_argument("--cppcheck", action="store_true", help="额外跑 cppcheck（需已安装）")
    parser.add_argument("--quiet", action="store_true")
    args = parser.parse_args(argv)

    build_dir = (ROOT / args.build_dir).resolve()
    entries = load_entries(build_dir)
    print(f"编译数据库: {build_dir / 'compile_commands.json'}（{len(entries)} 个编译单元）")

    failures: list[tuple[str, str]] = []
    for entry in entries:
        cmd = syntax_args(entry) + ["-fsyntax-only", str(entry.get("file", ""))]
        cwd = entry.get("directory") or str(ROOT)
        try:
            proc = subprocess.run(cmd, capture_output=True, text=True, cwd=cwd, timeout=120)
        except (OSError, subprocess.SubprocessError) as exc:
            failures.append((str(entry.get("file")), f"执行失败: {exc}"))
            continue
        if proc.returncode != 0:
            first = next((ln for ln in proc.stderr.splitlines() if "error:" in ln), "")
            failures.append((str(entry.get("file")), first or proc.stderr.strip()[:160]))
        elif not args.quiet:
            print(f"  [ok] {Path(str(entry.get('file'))).name}")

    cppcheck_rc = 0
    if args.cppcheck:
        exe = shutil.which("cppcheck")
        if not exe:
            print("[警告] 未找到 cppcheck，跳过静态分析（按未验证处理，不假装通过）")
        else:
            files = [str(e.get("file")) for e in entries]
            proc = subprocess.run(
                [exe, "--language=c++", "--enable=warning,style,performance",
                 "--error-exitcode=1", "--template={file}:{line}:{severity}:{message}",
                 "--quiet", *files],
                capture_output=True, text=True, cwd=str(ROOT), timeout=600)
            cppcheck_rc = proc.returncode
            lines = [ln for ln in proc.stderr.splitlines() if ln.strip()]
            print(f"cppcheck: {len(lines)} 条诊断（exit={cppcheck_rc}）")
            for ln in lines[:15]:
                print(f"  {ln}")

    print("-" * 60)
    if failures:
        print(f"✗ 存在 {len(failures)} 个编译错误（Problems 面板会标红）:")
        for path, msg in failures[:20]:
            print(f"  {path}: {msg}")
        return 1
    print("✓ 编译错误 0（Problems 面板等价校验通过）")
    if args.cppcheck and cppcheck_rc != 0:
        print("  （cppcheck 有诊断，按门禁 QUAL-003 的棘轮基线判定，不在此处阻断）")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
