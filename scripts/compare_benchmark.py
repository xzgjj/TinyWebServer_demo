#!/usr/bin/env python3
"""
基准测试结果比较脚本

用于比较两次基准测试结果，检测性能回归。
用法：
    python3 compare_benchmark.py baseline.json current.json [--threshold 0.1]
"""

import json
import sys
import argparse
from pathlib import Path

def load_metrics(json_file):
    """从JSON文件加载指标，返回字典 {指标名: 值}"""
    with open(json_file, 'r') as f:
        data = json.load(f)

    metrics = {}
    for metric in data.get('metrics', []):
        name = metric.get('name')
        value = metric.get('value')
        if name is not None and value is not None:
            metrics[name] = value

    return metrics

def compare_metrics(baseline, current, threshold=0.1):
    """
    比较指标，返回差异和回归检测结果

    threshold: 允许的性能下降阈值（比例），例如0.1表示10%
    返回: (regressions, improvements, unchanged)
    """
    regressions = []
    improvements = []
    unchanged = []

    # 关注的指标（值越大越好）
    larger_is_better = {'qps', 'throughput_mbps', 'successful_requests'}
    # 关注的指标（值越小越好）
    smaller_is_better = {'avg_latency', 'p50_latency', 'p90_latency', 'p99_latency', 'error_rate'}

    all_metrics = set(baseline.keys()) & set(current.keys())

    for metric in all_metrics:
        baseline_val = baseline[metric]
        current_val = current[metric]

        if baseline_val == 0:
            # 避免除以零
            change = 0.0
        else:
            change = (current_val - baseline_val) / abs(baseline_val)

        if metric in larger_is_better:
            # 值越大越好，负变化表示性能下降
            if change < -threshold:
                regressions.append((metric, baseline_val, current_val, change*100))
            elif change > threshold:
                improvements.append((metric, baseline_val, current_val, change*100))
            else:
                unchanged.append((metric, baseline_val, current_val, change*100))
        elif metric in smaller_is_better:
            # 值越小越好，正变化表示性能下降（延迟增加）
            if change > threshold:
                regressions.append((metric, baseline_val, current_val, change*100))
            elif change < -threshold:
                improvements.append((metric, baseline_val, current_val, change*100))
            else:
                unchanged.append((metric, baseline_val, current_val, change*100))
        else:
            # 未知指标，忽略或记录
            unchanged.append((metric, baseline_val, current_val, change*100))

    return regressions, improvements, unchanged

def main():
    parser = argparse.ArgumentParser(description='比较基准测试结果')
    parser.add_argument('baseline', help='基线结果JSON文件')
    parser.add_argument('current', help='当前结果JSON文件')
    parser.add_argument('--threshold', type=float, default=0.1,
                       help='性能回归阈值（比例，默认0.1即10%）')
    parser.add_argument('--fail-on-regression', action='store_true',
                       help='如果检测到回归，则退出码为非零')

    args = parser.parse_args()

    baseline_path = Path(args.baseline)
    current_path = Path(args.current)

    if not baseline_path.exists():
        print(f"错误: 基线文件不存在: {baseline_path}")
        sys.exit(1)
    if not current_path.exists():
        print(f"错误: 当前结果文件不存在: {current_path}")
        sys.exit(1)

    baseline_metrics = load_metrics(baseline_path)
    current_metrics = load_metrics(current_path)

    print("=" * 60)
    print("基准测试结果比较")
    print(f"基线: {baseline_path}")
    print(f"当前: {current_path}")
    print(f"阈值: {args.threshold*100:.1f}%")
    print("=" * 60)

    regressions, improvements, unchanged = compare_metrics(
        baseline_metrics, current_metrics, args.threshold)

    if regressions:
        print("\n❌ 性能回归检测到:")
        for metric, baseline_val, current_val, change_pct in regressions:
            print(f"  {metric}: {baseline_val:.2f} -> {current_val:.2f} ({change_pct:+.1f}%)")
    else:
        print("\n✅ 未检测到性能回归")

    if improvements:
        print("\n📈 性能提升:")
        for metric, baseline_val, current_val, change_pct in improvements:
            print(f"  {metric}: {baseline_val:.2f} -> {current_val:.2f} ({change_pct:+.1f}%)")

    if unchanged:
        print("\n📊 无明显变化指标:")
        for metric, baseline_val, current_val, change_pct in unchanged[:10]:  # 限制输出
            print(f"  {metric}: {baseline_val:.2f} -> {current_val:.2f} ({change_pct:+.1f}%)")
        if len(unchanged) > 10:
            print(f"  ... 还有 {len(unchanged) - 10} 个指标")

    print("\n" + "=" * 60)

    if args.fail_on_regression and regressions:
        print("检测到性能回归，退出码为1")
        sys.exit(1)
    else:
        sys.exit(0)

if __name__ == '__main__':
    main()