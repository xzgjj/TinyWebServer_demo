#!/usr/bin/env bash
# =============================================================================
# 多标准 × 多编译器构建矩阵验证（WSL2 与 MSYS2 UCRT64 通用）
#
# 用法:
#   bash scripts/verify_matrix.sh <cxx> <std> [tag]
#   bash scripts/verify_matrix.sh g++ 23 gcc23
#   bash scripts/verify_matrix.sh clang++ 17 clang17
#
# 退出码: 0 全绿 / 1 配置或构建失败
# 说明: 这是"证据脚本"——CI 与本地用同一套命令，避免"CI 绿本地红"。
# =============================================================================
set -u
cd "$(dirname "$0")/.." || exit 1

CXX="${1:-g++}"
STD="${2:-23}"
TAG="${3:-$(echo "$CXX$STD" | tr -c 'a-zA-Z0-9' '-')}"
BD="build/matrix-${TAG}"
JOBS="${JOBS:-8}"

echo "########## ${CXX} c++${STD} -> ${BD} ##########"
rm -rf "$BD"

cmake -S . -B "$BD" -G Ninja \
  -DCMAKE_CXX_COMPILER="$CXX" \
  -DCMAKE_BUILD_TYPE=Debug \
  -DTWS_CXX_STANDARD="$STD" \
  -DBUILD_TESTING=ON \
  -DBUILD_BENCHMARKS=ON \
  -DENABLE_ASAN=OFF > /tmp/tws_cfg.log 2>&1
cfg_rc=$?
echo "configure rc=$cfg_rc"
grep -E '源文件|测试目标|跳过' /tmp/tws_cfg.log | sed 's/^-- /  /'
if [ "$cfg_rc" -ne 0 ]; then
  tail -20 /tmp/tws_cfg.log
  exit 1
fi

cmake --build "$BD" --parallel "$JOBS" > /tmp/tws_build.log 2>&1
build_rc=$?
echo "build rc=$build_rc"
if [ "$build_rc" -ne 0 ]; then
  grep -E 'error:|FAILED:' /tmp/tws_build.log | head -15
  exit 1
fi
echo "warnings=$(grep -c 'warning:' /tmp/tws_build.log || true)"

# 只取 ctest 的判定行与标签汇总，避免 tail 把 "100% tests passed" 截掉
ctest --test-dir "$BD" --output-on-failure --timeout 120 > /tmp/tws_ctest.log 2>&1
ctest_rc=$?
grep -E 'tests passed|tests failed' /tmp/tws_ctest.log | tail -2
grep -E '^(portable|smoke|socket|integration) ' /tmp/tws_ctest.log
if [ "$ctest_rc" -ne 0 ]; then
  echo "--- ctest 失败明细（尾 20 行）---"
  tail -20 /tmp/tws_ctest.log
  exit 1
fi
exit 0
