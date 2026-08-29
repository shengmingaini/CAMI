#!/usr/bin/env bash
# TASK-041 · 跨进程集成与战斗性能回归 —— 本地验收脚本
# 自动生成：python tools/gen/build_tasks.py   （禁止手工编辑，改数据源后重新生成）
# 环境：Windows Git Bash / MSYS2 MinGW；g++ (MinGW MSYS2)；vcpkg manifest mode baseline aae277ac
# 用法：bash scripts/verify/task-041.sh
#      BUILD_TYPE=Debug  bash scripts/verify/task-041.sh
# 红线：不做网络操作、不推送 Git、不写数据库；任一步失败即非零退出。

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
source "$ROOT/scripts/verify/_common.sh"

begin_task "TASK-041" '跨进程集成与战斗性能回归'

# ---- 1. 前置任务门禁：必须全部 STATUS: DONE ----
require_tasks_done 025 030 033

# ---- 2. 交付物存在性 ----
require_files \
  'tools/qa/docs/REGRESSION.md' \
  'bench/combat_regression_lua.txt'

# ---- 3. 模块边界：公开头不得 include 内部 src/ ----
if [ -d "$ROOT/tools/qa/include" ]; then
  scan_forbidden 'tools/qa/include' '#include\s+["<][^">]*src/[^">]*'
fi

# ---- 4. 编译（本地 MinGW + vcpkg，CI 不作为验收依据） ----
cmake_build_both

# ---- 5. 单元测试（ctest 过滤执行） ----
run_ctest "$BUILD_TYPE" 'QA_E2E' 'QA_E2E'

# ---- 6. Benchmark 与性能阈值断言 ----
mkdir -p "$ROOT/bench"
run_bench "$BUILD_TYPE" 'bin/combat_bench' --matrix --lua-loaded --duration 60 --warmup 5 --out bench/combat_regression_lua.json
assert_metric 'bench/combat_regression_lua.txt' 'tick_p95_us' 'le' '5000'
assert_metric 'bench/combat_regression_lua.txt' 'tick_p99_us' 'le' '8000'

# ---- 7. 验收结论 ----
info "人工复核项（脚本无法自动判定，必须人工确认后勾选）："
info "  [ ] 1. 跨进程 E2E 一次完整跑通（Login→…→Persist），每阶段断言可观测"
info "  [ ] 2. TASK-030 economy_audit 五场景故障下资金守恒全过"
info "  [ ] 3. TASK-033 Lua 全量加载后重跑 TASK-025 1k 战斗矩阵：tick_p95 ≤ 5000、tick_p99 ≤ 8000，退化 < 5%"
info "  [ ] 4. 回归报告 REGRESSION.md 含 Lua 前/后对比与瓶颈定位"
info "  [ ] 5. 回归命令可重复执行，作为后续 Lua/战斗改动的常驻门禁"
info "  [ ] 6. Debug / Release 双构建通过，ctest -R QA_E2E 全绿"

end_task "TASK-041"
