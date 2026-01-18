# 完整流程（推荐） python3 tools.py all
# 只构建   python3 tools.py build
# 只测试  python3 tools.py test
# gdb 调试某个测试  python3 tools.py debug --target test_epoll_server

#!/usr/bin/env python3
# -*- coding: utf-8 -*-

#!/usr/bin/env python3
# -*- coding: utf-8 -*-

#!/usr/bin/env python3
# -*- coding: utf-8 -*-

#!/usr/bin/env python3
# -*- coding: utf-8 -*-

"""
Project build / test / debug helper.
Optimized for TinyWebServer_v1 full test suite.
"""

import argparse
import os
import shutil
import subprocess
import sys
from datetime import datetime
from pathlib import Path
from typing import List, Tuple
import concurrent.futures

ROOT_DIR = Path(__file__).resolve().parent
BUILD_DIR = ROOT_DIR / "build"
REPORT_FILE = ROOT_DIR / "report.md"
GDB_LOG = ROOT_DIR / "gdb.log"

# 必须与 CMakeLists.txt 中的目标名称严格匹配
TEST_EXECUTABLES = [
    "test_lifecycle",
    "test_single_connection",
    "test_multi_connection",
    "test_client_close",
    "test_backpressure",
    "test_stress",
    "test_main",
]

def clean():
    if BUILD_DIR.exists():
        shutil.rmtree(BUILD_DIR)
        print("Build directory removed.")

def cmake_configure() -> str:
    BUILD_DIR.mkdir(exist_ok=True)
    res = subprocess.run(["cmake", "-S", ".", "-B", "build"], 
                         capture_output=True, text=True)
    return res.stdout + res.stderr

def cmake_build() -> str:
    res = subprocess.run(["cmake", "--build", "build", "--parallel"], 
                         capture_output=True, text=True)
    return res.stdout + res.stderr

def run_single_test(test_name: str) -> Tuple[str, str, str]:
    exe_path = BUILD_DIR / test_name
    if not exe_path.exists():
        return (test_name, "FAIL", f"Executable not found: {exe_path}")

    try:
        # 设置10秒超时，防止测试中的死循环
        result = subprocess.run(
            [str(exe_path)],
            cwd=BUILD_DIR,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            timeout=10
        )
        status = "PASS" if result.returncode == 0 else "FAIL"
        return (test_name, status, result.stdout)
    except subprocess.TimeoutExpired as e:
        return (test_name, "TIMEOUT", f"Test timed out: {e.stdout}")
    except Exception as e:
        return (test_name, "ERROR", str(e))

def run_all_tests_parallel() -> List[Tuple[str, str, str]]:
    results = []
    with concurrent.futures.ThreadPoolExecutor() as executor:
        future_to_test = {executor.submit(run_single_test, name): name for name in TEST_EXECUTABLES}
        for future in concurrent.futures.as_completed(future_to_test):
            results.append(future.result())
    return results

def run_gdb(target: str) -> str:
    gdb_cmds = f"set logging file {GDB_LOG}\nset logging on\nrun\nbt\nquit\n"
    cmd_file = ROOT_DIR / ".gdb_cmds"
    cmd_file.write_text(gdb_cmds)
    
    res = subprocess.run(["gdb", "-batch", "-x", str(cmd_file), str(BUILD_DIR / target)],
                         capture_output=True, text=True)
    
    if cmd_file.exists():
        os.remove(cmd_file)
    return res.stdout

def write_report(conf, build, tests, gdb=""):
    with open(REPORT_FILE, "w") as f:
        f.write(f"# Build & Test Report\n\n**Generated:** {datetime.now().isoformat()}\n\n")
        f.write(f"## Configure Output\n```\n{conf}\n```\n\n")
        f.write(f"## Build Output\n```\n{build}\n```\n\n")
        f.write("## Test Results\n")
        for name, status, out in tests:
            f.write(f"### {name} - {status}\n```\n{out}\n```\n\n")
        if gdb:
            f.write(f"## GDB Debug Log\n```\n{gdb}\n```\n")

def main():
    parser = argparse.ArgumentParser()
    sub = parser.add_subparsers(dest="command", required=True)
    sub.add_parser("build")
    sub.add_parser("test")
    sub.add_parser("clean")
    sub.add_parser("all")
    debug_p = sub.add_parser("debug")
    debug_p.add_argument("--target", required=True)

    args = parser.parse_args()

    c_log, b_log, t_res, g_log = "", "", [], ""

    if args.command == "clean":
        clean()
    elif args.command == "build":
        cmake_configure(); cmake_build()
    elif args.command == "test":
        t_res = run_all_tests_parallel()
    elif args.command == "all":
        c_log = cmake_configure()
        b_log = cmake_build()
        t_res = run_all_tests_parallel()
        write_report(c_log, b_log, t_res)
    elif args.command == "debug":
        g_log = run_gdb(args.target)
        write_report("Skipped", "Skipped", [], g_log)

    if t_res:
        print("\n=== Test Summary ===")
        for name, status, _ in sorted(t_res):
            print(f"{name}: {status}")

if __name__ == "__main__":
    main()