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

#!/usr/bin/env python3
# -*- coding: utf-8 -*-

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
import traceback
from datetime import datetime
from pathlib import Path
from typing import List, Tuple, Optional

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
    "test_multithread_reactor",
    "test_basic",
    "test_log_bench",
]

class TestRunner:
    """测试运行器，增强错误处理和报告功能"""
    
    def __init__(self):
        self.results: List[Tuple[str, str, str]] = []
        self.build_log: str = ""
        self.errors: List[str] = []
        self.start_time = datetime.now()
    
    def log_error(self, msg: str):
        """记录错误"""
        self.errors.append(f"[{datetime.now().strftime('%H:%M:%S')}] {msg}")
        print(f"[Error] {msg}")
    
    def safe_run_test(self, name: str) -> Tuple[str, str, str]:
        """安全运行测试，捕获所有异常"""
        try:
            return self._run_test_impl(name)
        except Exception as e:
            error_msg = f"运行测试 {name} 时发生异常: {str(e)}"
            self.log_error(error_msg)
            return name, "CRASHED", f"{error_msg}\n\n堆栈跟踪:\n{traceback.format_exc()}"
    
    def _run_test_impl(self, name: str) -> Tuple[str, str, str]:
        """运行单个测试用例"""
        target_path = BUILD_DIR / name
        if not target_path.exists():
            return name, "NOT FOUND", f"二进制文件 {name} 不存在于 {target_path}"

        # 设置 ASan 环境变量
        env = os.environ.copy()
        env["ASAN_OPTIONS"] = "detect_leaks=1:halt_on_error=1:allocator_may_return_null=1"
        
        # 检查是否带有 ASan
        has_asan = False
        try:
            nm_res = subprocess.run(["nm", str(target_path)], capture_output=True, text=True)
            if "asan" in nm_res.stdout.lower():
                has_asan = True
        except: 
            pass

        # 智能设置超时时间
        timeout_map = {
            "test_log_bench": 300,      # 性能测试需要较长时间
            "test_stress": 180,         # 压力测试
            "test_backpressure": 180,    # 背压测试
            "test_multi_connection": 60, # 多连接测试
            "default": 30               # 默认超时
        }
        timeout = timeout_map.get(name, timeout_map["default"])
        
        # 决策：是否使用 Valgrind
        use_valgrind = (name in ["test_stress", "test_backpressure"]) and (not has_asan)
        
        cmd = []
        val_log = f"valgrind_{name}.log"
        if use_valgrind:
            cmd = ["valgrind", "--leak-check=full", f"--log-file={val_log}", str(target_path)]
        else:
            cmd = [str(target_path)]

        print(f"  -> 运行 {name} (超时: {timeout}s)...", end="", flush=True)
        try:
            result = subprocess.run(cmd, env=env, capture_output=True, text=True, timeout=timeout)
            status = "PASS" if result.returncode == 0 else "FAIL"
            
            output = result.stdout + result.stderr
            if use_valgrind and Path(val_log).exists():
                output += "\n\n[Valgrind Analysis]\n" + Path(val_log).read_text()
                Path(val_log).unlink()
                
            print(f" [{status}]")
            return name, status, output
            
        except subprocess.TimeoutExpired:
            print(" [TIMEOUT]")
            return name, "TIMEOUT", f"执行超过 {timeout} 秒，测试被终止"
            
        except KeyboardInterrupt:
            print(" [INTERRUPTED]")
            raise  # 重新抛出，让上层处理
            
        except Exception as e:
            print(" [ERROR]")
            return name, "ERROR", f"执行错误: {str(e)}"
    
    def generate_report(self, build_log: Optional[str] = None, 
                       interrupted: bool = False,
                       error: Optional[Exception] = None) -> bool:
        """生成最终 Markdown 报告，返回是否成功"""
        
        if build_log:
            self.build_log = build_log
            
        try:
            # 计算测试统计
            total = len(self.results)
            passed = sum(1 for _, status, _ in self.results if status == "PASS")
            failed = sum(1 for _, status, _ in self.results if status == "FAIL")
            other = total - passed - failed
            
            # 计算运行时间
            end_time = datetime.now()
            duration = (end_time - self.start_time).total_seconds()
            
            with open(REPORT_FILE, "w", encoding="utf-8") as f:
                f.write("# TinyWebServer V3 自动化审计报告\n\n")
                f.write(f"- **生成时间:** {end_time.strftime('%Y-%m-%d %H:%M:%S')}\n")
                f.write(f"- **运行时长:** {duration:.1f} 秒\n")
                f.write(f"- **模式:** Release (-O3) + AddressSanitizer\n")
                
                if interrupted:
                    f.write("- **状态:** ⚠️ 测试被用户中断\n")
                elif error:
                    f.write(f"- **状态:** ❌ 测试执行失败: {str(error)}\n")
                else:
                    f.write(f"- **状态:** {'✅ 完成' if failed == 0 else '⚠️ 有测试失败'}\n")
                
                f.write(f"- **统计:** {total} 个测试，{passed} 通过，{failed} 失败，{other} 其他\n\n")
                
                # 错误信息部分（如果有）
                if self.errors:
                    f.write("## ⚠️ 执行错误\n")
                    for err in self.errors:
                        f.write(f"- {err}\n")
                    f.write("\n")
                
                # 编译状态部分
                f.write("## 1. 编译状态\n")
                if self.build_log:
                    success = "成功" if "Error" not in self.build_log else "失败"
                    f.write(f"状态: **{success}**\n")
                    f.write("```text\n" + self.build_log[-1000:] + "\n```\n\n")
                else:
                    f.write("状态: 未记录编译日志\n\n")
                
                # 测试概览
                f.write("## 2. 测试概览\n")
                f.write("| 测试项 | 状态 | 详细链接 |\n| :--- | :--- | :--- |\n")
                for name, status, _ in self.results:
                    # 使用状态图标
                    status_icon = {
                        "PASS": "✅",
                        "FAIL": "❌", 
                        "TIMEOUT": "⏰",
                        "NOT FOUND": "🔍",
                        "ERROR": "💥",
                        "CRASHED": "💣"
                    }.get(status, "❓")
                    
                    f.write(f"| {name} | {status_icon} {status} | [查看详情](#{name}) |\n")
                
                f.write("\n## 3. 详细输出\n")
                for name, status, out in self.results:
                    f.write(f'### <a name="{name}"></a>{name}\n')
                    f.write(f"**状态:** {status}\n\n")
                    f.write("```text\n")
                    
                    # 限制输出长度，避免报告过大
                    max_output_length = 5000
                    if out and len(out) > max_output_length:
                        f.write(out[:max_output_length])
                        f.write(f"\n\n... (输出过长，已截断，共 {len(out)} 字符)")
                    else:
                        f.write(out if out else "无输出")
                    
                    f.write("\n```\n\n")
                
                # 建议和总结
                f.write("## 4. 总结与建议\n")
                
                if failed > 0:
                    f.write("### ❌ 发现问题\n")
                    failed_tests = [name for name, status, _ in self.results if status == "FAIL"]
                    f.write(f"- 以下测试失败: {', '.join(failed_tests)}\n")
                    f.write("- 建议检查网络连接、端口冲突或服务器配置\n")
                
                if any(status in ["TIMEOUT", "ERROR", "CRASHED"] for _, status, _ in self.results):
                    f.write("### ⚠️ 异常情况\n")
                    for name, status, _ in self.results:
                        if status in ["TIMEOUT", "ERROR", "CRASHED"]:
                            f.write(f"- {name}: {status}\n")
                
                if interrupted:
                    f.write("### ⏸️ 测试被中断\n")
                    f.write("- 用户按下了 Ctrl+C\n")
                    f.write("- 部分测试可能没有完成\n")
                    f.write("- 建议重新运行完整的测试流程\n")
                
                if passed == total and not interrupted and not error:
                    f.write("### ✅ 所有测试通过\n")
                    f.write("- 恭喜！所有测试都通过了\n")
                    f.write("- 项目质量良好\n")
            
            print(f"[报告] 报告已生成: {REPORT_FILE}")
            return True
            
        except Exception as e:
            print(f"[错误] 生成报告失败: {e}")
            # 尝试生成简单的错误报告
            try:
                with open(REPORT_FILE, "w", encoding="utf-8") as f:
                    f.write("# 报告生成失败\n\n")
                    f.write(f"错误: {str(e)}\n")
                    f.write(f"时间: {datetime.now()}\n")
                print(f"[报告] 已创建错误报告")
            except:
                print(f"[错误] 无法创建任何报告")
            return False

def main():
    """主函数，增强错误处理"""
    runner = TestRunner()
    
    parser = argparse.ArgumentParser(description="V3 Build & Test Tools")
    parser.add_argument("command", choices=["build", "test", "clean", "all"], 
                       help="Command to execute")
    args = parser.parse_args()
    
    try:
        if args.command == "clean":
            clean()
        
        elif args.command == "build":
            runner.build_log = cmake_configure() + "\n" + cmake_build()
            
        elif args.command == "test":
            if not BUILD_DIR.exists():
                runner.log_error("构建目录不存在，请先运行 'build'")
                print("[错误] 构建目录不存在。请运行: python3 tools.py build")
                return
            
            print(f"[测试] 开始运行 {len(TEST_EXECUTABLES)} 个测试...")
            for t in TEST_EXECUTABLES:
                runner.results.append(runner.safe_run_test(t))
            
            # 生成报告
            if runner.generate_report("手动测试运行"):
                print(f"\n[完成] 报告已生成: {REPORT_FILE}")
                # 显示简要统计
                passed = sum(1 for _, status, _ in runner.results if status == "PASS")
                total = len(runner.results)
                print(f"[统计] {passed}/{total} 个测试通过")
        
        elif args.command == "all":
            print("[开始] 执行完整流程...")
            clean()
            runner.build_log = cmake_configure() + "\n" + cmake_build()
            
            print(f"[测试] 开始运行 {len(TEST_EXECUTABLES)} 个测试...")
            for t in TEST_EXECUTABLES:
                runner.results.append(runner.safe_run_test(t))
            
            # 生成报告
            if runner.generate_report():
                print(f"\n[完成] 报告已生成: {REPORT_FILE}")
                # 显示简要统计
                passed = sum(1 for _, status, _ in runner.results if status == "PASS")
                total = len(runner.results)
                print(f"[统计] {passed}/{total} 个测试通过")
    
    except KeyboardInterrupt:
        print("\n[中断] 用户中断了程序")
        # 即使被中断也生成报告
        runner.generate_report(interrupted=True)
        print(f"[报告] 中断报告已生成: {REPORT_FILE}")
        
    except Exception as e:
        print(f"\n[崩溃] 程序发生未处理异常: {e}")
        traceback.print_exc()
        # 即使崩溃也尝试生成报告
        runner.generate_report(error=e)
        print(f"[报告] 错误报告已生成: {REPORT_FILE}")
        sys.exit(1)

if __name__ == "__main__":
    main()