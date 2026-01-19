# 完整流程（推荐） python3 tools.py all
# 只构建   python3 tools.py build
# 只测试  python3 tools.py test
# gdb 调试某个测试  python3 tools.py debug --target test_epoll_server


# python3 tools.py build --mode perf --clean
# python3 tools.py test -only test_stress


#!/usr/bin/env python3
# -*- coding: utf-8 -*-

import argparse
import os
import shutil
import subprocess
import sys
from pathlib import Path

ROOT_DIR = Path(__file__).resolve().parent
BUILD_DIR = ROOT_DIR / "build"

# 预定义的测试目标（需与 CMake 一致）
ALL_TESTS = [
    "test_timer", "test_lifecycle", "test_single_connection",
    "test_multi_connection", "test_stress"
]

def run_cmd(cmd):
    print(f">> 执行: {' '.join(cmd)}")
    try:
        subprocess.run(cmd, cwd=ROOT_DIR, check=True)
    except subprocess.CalledProcessError:
        print("\n[错误] 命令执行失败，程序退出。")
        sys.exit(1)

def build(mode="Debug", clean_first=False):
    """
    mode: "Debug" (开启 ASan, -O0) 或 "Release" (性能模式, -O3)
    """
    if clean_first and BUILD_DIR.exists():
        print(f">> 清理构建目录...")
        shutil.rmtree(BUILD_DIR)

    if not BUILD_DIR.exists():
        BUILD_DIR.mkdir()

    print(f"\n[构建模式] {mode.upper()} " + ("(含 ASan)" if mode == "Debug" else "(性能优化)"))

    # 核心：通过 -DCMAKE_BUILD_TYPE 传递模式
    run_cmd(["cmake", "-S", ".", "-B", "build", f"-DCMAKE_BUILD_TYPE={mode}"])
    run_cmd(["cmake", "--build", "build", "-j"])

def test(targets=None):
    target_list = targets if targets else ALL_TESTS
    for t in target_list:
        exe = BUILD_DIR / t
        if exe.exists():
            print(f"\n>> 运行测试: {t}")
            # 注意：如果开启了 ASan，若有内存泄漏，程序会以非 0 状态码退出
            subprocess.run([str(exe)], check=True)
        else:
            print(f"[跳过] 未找到可执行文件: {t}")

def debug(target):
    """使用 GDB 调试指定目标"""
    exe = BUILD_DIR / target
    if not exe.exists():
        print(f"[错误] 可执行文件不存在: {exe}")
        sys.exit(1)

    print(f">> 使用 GDB 调试: {target}")
    run_cmd(["gdb", str(exe)])

def clean():
    """清理构建目录"""
    if BUILD_DIR.exists():
        print(f">> 清理构建目录: {BUILD_DIR}")
        shutil.rmtree(BUILD_DIR)
        print("已清理。")
    else:
        print(f">> 构建目录不存在: {BUILD_DIR}")

def main():
    parser = argparse.ArgumentParser(description="TinyWebServer 辅助工具")
    # 命令：all, build, test, debug, clean
    subparsers = parser.add_subparsers(dest="command")

    # 公共参数：模式切换
    def add_mode_arg(p):
        p.add_argument("-m", "--mode", choices=["debug", "perf"], default="debug",
                       help="debug: 开启 ASan (-O0); perf: 性能模式 (-O3, 无 ASan)")

    # all
    all_p = subparsers.add_parser("all", help="清理、构建并运行所有测试")
    add_mode_arg(all_p)

    # build
    build_p = subparsers.add_parser("build", help="仅构建项目")
    add_mode_arg(build_p)
    build_p.add_argument("-clean", action="store_true", help="构建前清理")

    # test
    test_p = subparsers.add_parser("test", help="运行测试")
    test_p.add_argument("-only", nargs="+", help="指定运行的测试名（可多个）")

    # debug (GDB)
    debug_p = subparsers.add_parser("debug", help="使用 GDB 调试指定目标")
    debug_p.add_argument("-target", required=True, help="可执行文件名")

    # clean (新添加的命令)
    clean_p = subparsers.add_parser("clean", help="清理构建目录")

    args = parser.parse_args()

    # 映射模式名称
    cmake_mode = "Debug" if getattr(args, "mode", "debug") == "debug" else "Release"

    if args.command == "all":
        build(cmake_mode, clean_first=True)
        test()
    elif args.command == "build":
        build(cmake_mode, args.clean)
    elif args.command == "test":
        # 如果指定了 -only 参数，只运行指定的测试
        if args.only:
            test(args.only)
        else:
            test()  # 运行所有测试
    elif args.command == "debug":
        debug(args.target)
    elif args.command == "clean":
        clean()
    else:
        parser.print_help()

if __name__ == "__main__":
    main()
