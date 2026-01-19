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

#!/usr/bin/env python3
# -*- coding: utf-8 -*-

#!/usr/bin/env python3
# -*- coding: utf-8 -*-

import argparse
import os
import shutil
import subprocess
import sys
from datetime import datetime
from pathlib import Path
from typing import List, Tuple

ROOT_DIR = Path(__file__).resolve().parent
BUILD_DIR = ROOT_DIR / "build"
REPORT_FILE = ROOT_DIR / "report.md"

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
    """清理构建目录和报告"""
    if BUILD_DIR.exists():
        shutil.rmtree(BUILD_DIR)
    if REPORT_FILE.exists():
        REPORT_FILE.unlink()
    for f in ROOT_DIR.glob("valgrind_*.log"):
        f.unlink()
    print("[Clean] Done. Build dir and old reports removed.")

def cmake_configure():
    """配置 CMake (Release 模式以启用 -O3)"""
    BUILD_DIR.mkdir(exist_ok=True)
    print(f"[Configure] Running CMake in {BUILD_DIR}...")
    # 强制指定 Release 模式以激活 CMakeLists.txt 中的 -O3 + ASan 逻辑
    res = subprocess.run(["cmake", "-DCMAKE_BUILD_TYPE=Release", ".."], 
                         cwd=BUILD_DIR, capture_output=True, text=True)
    if res.returncode != 0:
        print("[Error] CMake Configuration Failed!")
        print(res.stderr)
        sys.exit(1)
    return res.stdout

def cmake_build():
    """编译项目"""
    print("[Build] Compiling project with -j (Parallel)...")
    res = subprocess.run(["make", "-j"], cwd=BUILD_DIR, capture_output=True, text=True)
    if res.returncode != 0:
        print("[Error] Build Failed!")
        print(res.stderr)
        # 不退出，以便将错误写入报告
    return res.stdout + res.stderr

def run_test(name: str) -> Tuple[str, str, str]:
    """运行单个测试用例"""
    target_path = BUILD_DIR / name
    if not target_path.exists():
        return name, "NOT FOUND", f"Binary {name} not found."

    # 设置 ASan 环境变量：发现错误立即退出并打印堆栈
    env = os.environ.copy()
    env["ASAN_OPTIONS"] = "detect_leaks=1:halt_on_error=1:allocator_may_return_null=1"
    
    # 检查当前二进制是否带有 ASan (带有 asan 符号时，严禁使用 valgrind)
    has_asan = False
    try:
        nm_res = subprocess.run(["nm", str(target_path)], capture_output=True, text=True)
        if "asan" in nm_res.stdout.lower():
            has_asan = True
    except: pass

    # 决策：是否使用 Valgrind
    # 如果有 ASan 就不加 Valgrind，否则压力测试必崩
    use_valgrind = (name in ["test_stress", "test_backpressure"]) and (not has_asan)
    
    cmd = []
    val_log = f"valgrind_{name}.log"
    if use_valgrind:
        cmd = ["valgrind", "--leak-check=full", f"--log-file={val_log}", str(target_path)]
    else:
        cmd = [str(target_path)]

    print(f"  -> Running {name}...", end="", flush=True)
    try:
        # 增加超时，防止压力测试死锁
        result = subprocess.run(cmd, env=env, capture_output=True, text=True, timeout=120)
        status = "PASS" if result.returncode == 0 else "FAIL"
        
        output = result.stdout + result.stderr
        if use_valgrind and Path(val_log).exists():
            output += "\n\n[Valgrind Analysis]\n" + Path(val_log).read_text()
            Path(val_log).unlink()
            
        print(f" [{status}]")
        return name, status, output
    except subprocess.TimeoutExpired:
        print(" [TIMEOUT]")
        return name, "TIMEOUT", "Execution exceeded 120s."
    except Exception as e:
        print(" [ERROR]")
        return name, "ERROR", str(e)

def generate_report(build_log: str, results: List[Tuple[str, str, str]]):
    """生成最终 Markdown 报告"""
    with open(REPORT_FILE, "w", encoding="utf-8") as f:
        f.write("# TinyWebServer V3 自动化审计报告\n\n")
        f.write(f"- **时间:** {datetime.now().strftime('%Y-%m-%d %H:%M:%S')}\n")
        f.write("- **模式:** Release (-O3) + AddressSanitizer\n\n")
        
        f.write("## 1. 编译状态\n")
        success = "成功" if "Error" not in build_log else "失败"
        f.write(f"状态: **{success}**\n")
        f.write("```text\n" + build_log[-1000:] + "\n```\n\n")
        
        f.write("## 2. 测试概览\n")
        f.write("| 测试项 | 状态 | 详细链接 |\n| :--- | :--- | :--- |\n")
        for name, status, _ in results:
            f.write(f"| {name} | {status} | [查看详情](#{name}) |\n")
        
        f.write("\n## 3. 详细输出\n")
        for name, status, out in results:
            f.write(f"### <a name=\"{name}\"></a>{name} ({status})\n")
            f.write("```text\n" + (out if out else "No output.") + "\n```\n\n")

def main():
    parser = argparse.ArgumentParser(description="V3 Build & Test Tools")
    parser.add_argument("command", choices=["build", "test", "clean", "all"], 
                        help="Command to execute")
    args = parser.parse_args()

    if args.command == "clean":
        clean()
    
    elif args.command == "build":
        cmake_configure()
        cmake_build()
        
    elif args.command == "test":
        if not BUILD_DIR.exists():
            print("[Error] Build directory not found. Run 'build' first.")
            return
        results = []
        for t in TEST_EXECUTABLES:
            results.append(run_test(t))
        generate_report("Manual Test Run", results)
        print(f"\n[Done] Report: {REPORT_FILE}")

    elif args.command == "all":
        clean()
        cmake_configure()
        b_log = cmake_build()
        results = []
        for t in TEST_EXECUTABLES:
            results.append(run_test(t))
        generate_report(b_log, results)
        print(f"\n[Done] Report: {REPORT_FILE}")

if __name__ == "__main__":
    main()