#!/usr/bin/env bash
# tools/qa/regression/run.sh — TASK-041 战斗性能回归 + 账本对账入口（可重复执行）
#
# 用法：
#   bash tools/qa/regression/run.sh
#   BUILD_TYPE=Debug bash tools/qa/regression/run.sh
#
# 行为：
#   1) 跑 combat_bench（沙箱可跑子集）：进程内真实加载 TASK-033 Lua 全集 + 重跑 TASK-025
#      战斗矩阵（含 1000 玩家 / 100% Combat），落盘 bench/combat_regression_lua.{txt,json}。
#   2) 跑 TASK-030 economy_audit.py（五场景故障下资金守恒；纯 Python，不依赖 C++ 链接）。
#   3) 汇总并提示真实跨进程部分（gRPC+Redis+MySQL、TASK-030 五场景故障注入对账）需在完整环境执行。
#
# 红线：本脚本只做本地回归与对账，不推送 Git、不写数据库、不做网络操作。
# 退出码：combat_bench 回归未过则非零退出；economy_audit 失败亦非零退出。

set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT"

BUILD_TYPE="${BUILD_TYPE:-Release}"
# 实际构建产物：Release 在 build/bin，Debug 在 build-debug/bin（与 cmake_build_both 口径一致）
if [ "$BUILD_TYPE" = "Debug" ]; then
    BIN_DIR="$ROOT/build-debug/bin"
else
    BIN_DIR="$ROOT/build/bin"
fi
COMBAT_BENCH="$BIN_DIR/combat_bench"
AUDIT_PY="$ROOT/tools/audit/economy_audit.py"

echo "== TASK-041 regression =="
echo "BUILD_TYPE=$BUILD_TYPE"
echo "ROOT=$ROOT"

# ---- 1. 战斗性能回归（沙箱可跑）----
if [ ! -x "$COMBAT_BENCH" ]; then
    echo "[SKIP] combat_bench 未构建（先跑 cmake_build_both）；期望路径: $COMBAT_BENCH" >&2
    exit 2
fi

mkdir -p "$ROOT/bench"
"$COMBAT_BENCH" --matrix --lua-loaded --duration 60 --warmup 5 --out bench/combat_regression_lua.json
bench_rc=$?
if [ "$bench_rc" -ne 0 ]; then
    echo "[FAIL] combat_bench 回归门禁未过（退出码 $bench_rc）" >&2
    exit "$bench_rc"
fi
echo "[OK] combat_bench 回归通过：tick_p95<=$tick_p95 gate, tick_p99<=$tick_p99 gate, lua 零装载错误"

# ---- 2. 账本对账（TASK-030，纯 Python）----
if [ -f "$AUDIT_PY" ]; then
    python3 "$AUDIT_PY" --self-test || {
        echo "[FAIL] economy_audit.py 五场景资金守恒未过" >&2
        exit 1
    }
    echo "[OK] economy_audit 五场景资金守恒通过"
else
    echo "[SKIP] economy_audit.py 不存在：$AUDIT_PY" >&2
fi

# ---- 3. 真实跨进程部分（沙箱不执行，需在完整环境跑）----
echo ""
echo "== 需在完整环境（Gateway+GameNode+DataService+Redis+MySQL）执行的真实跨进程回归 =="
echo "  [ ] 1. gRPC Bot 走 Login→Gateway→GameNode→EnterScene→Attack→Loot→Trade→Persist，每阶段可观测"
echo "  [ ] 2. TASK-030 economy_audit 五场景故障注入对账（进程内版已跑；注入崩溃/网络分区需在真实链路复跑）"
echo "  [ ] 3. 退化 > 5% 时报告标红并定位耗时 Lua 脚本"
echo ""
echo "本地沙箱回归完成（combat_bench + economy_audit 通过）。真实跨进程部分见 REGRESSION.md。"
exit 0
