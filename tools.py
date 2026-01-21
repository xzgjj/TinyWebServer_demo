# 完整流程（推荐） python3 tools.py all
# 只构建   python3 tools.py build
# 只测试  python3 tools.py test
# gdb 调试某个测试  python3 tools.py debug --target test_epoll_server


# python3 tools.py build --mode perf --clean
# python3 tools.py test -only test_stress


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
from pathlib import Path
import re
from datetime import datetime

# 路径配置
ROOT_DIR = Path(__file__).resolve().parent
BUILD_DIR = ROOT_DIR / "build"
LOG_DIR = BUILD_DIR / "test_logs"

def read_test_targets_from_cmake():
    """从CMakeLists.txt中解析测试目标列表"""
    cmake_file = ROOT_DIR / "CMakeLists.txt"
    test_targets = []
    
    if not cmake_file.exists():
        print(f"[警告] 未找到CMakeLists.txt文件: {cmake_file}")
        return ["test_timer", "test_lifecycle", "test_single_connection", "test_multi_connection", "test_stress"]
    
    try:
        with open(cmake_file, 'r', encoding='utf-8') as f:
            content = f.read()
            # 匹配 set(INTEGRATION_TESTS ...) 块
            match = re.search(r'set\(INTEGRATION_TESTS(.*?)\)', content, re.DOTALL)
            if match:
                targets = match.group(1).split()
                test_targets.extend([t.strip() for t in targets if t.strip()])
    except Exception as e:
        print(f"[错误] 解析CMakeLists.txt失败: {e}")
    
    return list(set(test_targets))

def setup_build_dir(clean=False):
    if clean and BUILD_DIR.exists():
        shutil.rmtree(BUILD_DIR)
    BUILD_DIR.mkdir(parents=True, exist_ok=True)
    LOG_DIR.mkdir(parents=True, exist_ok=True)

def build(mode="Debug"):
    setup_build_dir()
    print(f">> 开始构建项目 [模式: {mode}]...")
    try:
        subprocess.run(["cmake", f"-DCMAKE_BUILD_TYPE={mode}", ".."], cwd=str(BUILD_DIR), check=True)
        subprocess.run(["make", "-j4"], cwd=str(BUILD_DIR), check=True)
        return True
    except subprocess.CalledProcessError:
        print(">> 构建失败！")
        return False

def save_test_log(name, stdout, stderr, ret_code):
    log_file = LOG_DIR / f"{name}.log"
    with open(log_file, "w", encoding="utf-8") as f:
        f.write(f"--- TEST: {name} ---\n")
        f.write(f"--- RETURN CODE: {ret_code} ---\n\n")
        f.write("--- STDOUT ---\n")
        f.write(stdout)
        f.write("\n\n--- STDERR ---\n")
        f.write(stderr)
    return log_file

def generate_audit_report(passed, failed):
    """
    生成增强版审计报告
    1. 位置移动到根目录
    2. 增加性能指标和错误深度解析
    """
    report_path = ROOT_DIR / "audit_report.md" # 修改：生成到根目录
    
    with open(report_path, "w", encoding="utf-8") as f:
        f.write("# TinyWebServer 自动化审计与性能报告\n\n")
        f.write(f"- **生成时间:** {datetime.now().strftime('%Y-%m-%d %H:%M:%S')}\n")
        status_str = "❌ 存在失败项" if failed else "✅ 全部通过"
        f.write(f"- **项目状态:** {status_str}\n")
        f.write(f"- **测试概览:** 通过 {len(passed)}, 失败 {len(failed)}\n\n")
        
        f.write("## 详细测试概览\n")
        # 扩展列：增加“性能指标/错误摘要”
        f.write("| 测试项 | 状态 | 关键指标/错误摘要 | 详细日志 |\n")
        f.write("| :--- | :--- | :--- | :--- |\n")
        
        for name, metrics in passed:
            f.write(f"| {name} | ✅ 通过 | {metrics} | [Log](./build/test_logs/{name}.log) |\n")
        
        for name, reason, summary in failed:
            f.write(f"| {name} | ❌ {reason} | `<span style='color:red'>{summary}</span>` | [Log](./build/test_logs/{name}.log) |\n")
            
        f.write("\n\n---\n*注：性能指标从标准输出中实时提取。若出现失败项(-6)，请检查日志中的 ASan 内存审计报告。*")
    
    print(f"\n>> 审计报告已更新: {report_path}")

def find_test_executable(target):
    # 递归查找可执行文件
    for path in BUILD_DIR.rglob(target):
        if path.is_file() and os.access(path, os.X_OK):
            return path
    return None

def test(targets=None):
    all_targets = read_test_targets_from_cmake()
    target_list = targets if targets else all_targets
    
    passed_tests = []
    failed_tests = []

    # 启用 ASan 符号化输出环境变量
    env = os.environ.copy()
    env["ASAN_OPTIONS"] = "symbolize=1:break_on_error=1"

    for t in target_list:
        exe = find_test_executable(t)
        if not exe:
            print(f"[跳过] 未找到测试目标: {t}")
            continue

        print(f">> 正在运行 {t}...", end=" ", flush=True)
        try:
            # 增加超时处理
            result = subprocess.run(
                [str(exe)], 
                capture_output=True, 
                text=True, 
                timeout=60, 
                env=env,
                cwd=str(BUILD_DIR)
            )
            
            save_test_log(t, result.stdout, result.stderr, result.returncode)
            
            # 解析关键指标 (提取包含 QPS, logs/sec, ms, PASSED 等关键词的行)
            combined_output = result.stdout + "\n" + result.stderr
            metrics = "N/A"
            for line in combined_output.split('\n'):
                # 优先级匹配性能数据
                if any(kw in line.upper() for kw in ["QPS", "LOGS/SEC", "LATENCY", "REQUESTS/S", "THROUGHPUT"]):
                    metrics = line.strip()
                    break
                elif "PASSED" in line.upper():
                    metrics = "Success"

            if result.returncode == 0:
                print("✅")
                passed_tests.append((t, metrics))
            else:
                reason = f"失败({result.returncode})"
                # 尝试从 stderr 提取 ASan 错误首行
                err_summary = "Aborted (Check Logs)"
                if "AddressSanitizer" in result.stderr:
                    reason = "内存错误(ASan)"
                    for line in result.stderr.split('\n'):
                        if "ERROR: AddressSanitizer" in line:
                            err_summary = line.strip()
                            break
                elif result.stderr.strip():
                    err_summary = result.stderr.strip().split('\n')[-1]
                
                print(f"❌ [{reason}]")
                failed_tests.append((t, reason, err_summary))

        except subprocess.TimeoutExpired as e:
            print("⏳ [超时]")
            # 超时也尝试保存已产生的日志
            stdout = e.stdout.decode() if e.stdout else ""
            stderr = e.stderr.decode() if e.stderr else ""
            save_test_log(t, stdout, stderr, "TIMEOUT")
            failed_tests.append((t, "超时", "进程运行超过45秒限制"))
        except Exception as e:
            print(f"💥 [异常: {e}]")
            failed_tests.append((t, "执行异常", str(e)))

    generate_audit_report(passed_tests, failed_tests)
    return len(failed_tests) == 0

def debug(target):
    exe = find_test_executable(target)
    if exe:
        subprocess.run(["gdb", str(exe)])
    else:
        print(f"[错误] 未找到可调试目标: {target}")

def clean():
    if BUILD_DIR.exists():
        print(f">> 清理构建目录: {BUILD_DIR}")
        shutil.rmtree(BUILD_DIR)

# tools.py 优化片段

def extract_rich_info(name, stdout, stderr, is_passed):
    """
    深度提取函数：在报告生成阶段，从输出中挖掘更多有效信息
    """
    combined = stdout + "\n" + stderr
    info = []

    if is_passed:
        # 1. 提取性能指标 (增加更多匹配模式)
        patterns = [
            r"(\d+\.?\d* logs/sec)",         # 日志速率
            r"(QPS: \d+)",                   # 每秒查询
            r"(Latency: \d+\.?\d*ms)",       # 延迟
            r"(Total: \d+ bytes)",           # 吞吐量
            r"PASSED \((.*)\)"               # 捕获括号内的自定义成功说明
        ]
        for p in patterns:
            match = re.search(p, combined)
            if match: info.append(match.group(1))
        
        # 2. 提取并发规模
        if "connection" in name:
            conn_match = re.search(r"with (\d+) connections", combined)
            if conn_match: info.append(f"Conns: {conn_match.group(1)}")
            
        return " | ".join(info) if info else "Success (No metrics)"

    else:
        # 3. 提取错误上下文 (针对你的 IsInLoopThread 报错)
        if "Assertion `IsInLoopThread()'" in combined:
            # 尝试定位是哪个类触发的
            return "Thread Safety Error: EventLoop called from illegal thread"
        
        if "AddressSanitizer" in combined:
            # 提取具体的内存错误类型
            asan_match = re.search(r"ERROR: AddressSanitizer: ([\w-]+)", combined)
            return f"ASan: {asan_match.group(1)}" if asan_match else "Memory Error"

        # 4. 超时项提取最后一行有效日志，判断进度
        lines = [l for l in stdout.split('\n') if l.strip()]
        if lines:
            return f"Last Msg: {lines[-1][:50]}..."
            
        return "Aborted"

# 修改 generate_audit_report 中的循环逻辑
# for name, stdout, stderr, ret in raw_results:
#     metrics = extract_rich_info(name, stdout, stderr, ret == 0)
#     ... 写入表格 ...


def main():
    parser = argparse.ArgumentParser(description="TinyWebServer 辅助工具 - 性能审计版")
    subparsers = parser.add_subparsers(dest="command")
    
    add_m = lambda p: p.add_argument("-m", "--mode", choices=["debug", "perf"], default="debug", help="构建模式")
    
    all_p = subparsers.add_parser("all", help="全流程：清理+构建+测试")
    add_m(all_p)
    
    build_p = subparsers.add_parser("build", help="仅执行构建")
    add_m(build_p)
    build_p.add_argument("-clean", action="store_true", help="构建前清理")
    
    test_p = subparsers.add_parser("test", help="运行测试并生成审计报告")
    test_p.add_argument("-only", nargs="+", help="指定运行的测试项名称")
    
    debug_p = subparsers.add_parser("debug", help="使用GDB调试特定目标")
    debug_p.add_argument("-target", required=True, help="测试目标名称")
    
    subparsers.add_parser("clean", help="仅清理构建目录")

    args = parser.parse_args()
    
    if args.command == "all":
        clean()
        mode = "Debug" if args.mode == "debug" else "Release"
        if build(mode): test()
    elif args.command == "build":
        if args.clean: clean()
        mode = "Debug" if args.mode == "debug" else "Release"
        build(mode)
    elif args.command == "test":
        test(args.only)
    elif args.command == "debug":
        debug(args.target)
    elif args.command == "clean":
        clean()
    else:
        parser.print_help()

if __name__ == "__main__":
    main()