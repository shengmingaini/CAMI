#!/usr/bin/env bash
# TASK-013 · Simulation Scheduler（20Hz 固定 Tick） —— 本地验收脚本
# 自动生成：python tools/gen/build_tasks.py   （禁止手工编辑，改数据源后重新生成）
# 环境：Windows Git Bash / MSYS2 MinGW；g++ (MinGW MSYS2)；vcpkg manifest mode baseline aae277ac
# 用法：bash scripts/verify/task-013.sh
#      BUILD_TYPE=Debug  bash scripts/verify/task-013.sh
# 红线：不做网络操作、不推送 Git、不写数据库；任一步失败即非零退出。

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
source "$ROOT/scripts/verify/_common.sh"

begin_task "TASK-013" 'Simulation Scheduler（20Hz 固定 Tick）'

# ---- 1. 前置任务门禁：必须全部 STATUS: DONE ----
require_tasks_done 003 004 012

# ---- 2. 交付物存在性 ----
require_files \
  'server/gamenode/scheduler/include/mmo/game/sched/simulation_scheduler.h'

# ---- 3. 模块边界：公开头不得 include 内部 src/ ----
if [ -d "$ROOT/server/gamenode/scheduler/include" ]; then
  scan_forbidden 'server/gamenode/scheduler/include' '#include\s+["<][^">]*src/[^">]*'
fi

# ---- 4. 编译（本地 MinGW + vcpkg，CI 不作为验收依据） ----
cmake_build_both

# ---- 5. 单元测试（ctest 过滤执行） ----
run_ctest "$BUILD_TYPE" 'Sched_Sim' 'Sched_Sim'

# ---- 6. Benchmark 与性能阈值断言 ----
mkdir -p "$ROOT/bench"
run_bench "$BUILD_TYPE" 'bin/sched_sim_bench' --ticks 12000
assert_metric 'bench/sched_sim.txt' 'tick_overhead_ns' 'le' '20000'
assert_metric 'bench/sched_sim.txt' 'drift_us_per_10min' 'le' '50000'

# ---- 7. 验收结论 ----
info "人工复核项（脚本无法自动判定，必须人工确认后勾选）："
info "  [ ] 1. 八阶段按固定顺序执行（集成测试用 mock 记录调用序断言）"
info "  [ ] 2. **每个阶段都有独立耗时统计**，且各阶段之和 ≈ 总 Tick 耗时（误差 < 5%）"
info "  [ ] 3. 固定 20Hz：10 分钟长稳 Tick 数 = 12000 ± 5，无累积漂移"
info "  [ ] 4. CatchUp 限幅生效（注入 5 秒空档，只补 3 个 Tick）"
info "  [ ] 5. 单阶段异常不导致整个 Tick 崩溃（单测覆盖）"
info "  [ ] 6. 重复注册同一 Phase 返回错误，不静默覆盖"
info "  [ ] 7. Debug / Release 双构建通过，ctest -R Sched_Sim 全绿"

end_task "TASK-013"
