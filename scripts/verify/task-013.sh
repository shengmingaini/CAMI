#!/usr/bin/env bash
# TASK-013 · Simulation Scheduler（20Hz 固定 Tick）—— 本地验收脚本
# 自动生成：python tools/gen/build_tasks.py   （禁止手工编辑，改数据源后重新生成）
# 环境：Windows Git Bash / MSYS2 MinGW；g++ (MinGW MSYS2)；vcpkg manifest mode baseline aae277ac
# 用法：bash scripts/verify/task-013.sh
#      BUILD_TYPE=Debug  bash scripts/verify/task-013.sh
# 红线：不做网络操作、不推送 Git、不写数据库；任一步失败即非零退出。
#
# 注意：本仓库 build 缓存为 Ninja，必须注入 GENERATOR=Ninja（默认 MinGW Makefiles 会与
#       既有 Ninja 缓存冲突），并注入 MMO_PROJECT_ROOT / MMO_TASKS_DIR / VCPKG_ROOT / MinGW PATH。

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
source "$ROOT/scripts/verify/_common.sh"

begin_task "TASK-013" 'Simulation Scheduler'

# ---- 1. 前置任务门禁：必须全部 STATUS: DONE ----
require_tasks_done 003 004 012

# ---- 2. 交付物存在性 ----
require_files \
  'server/gamenode/scheduler/include/mmo/game/sched/tick_phase.h' \
  'server/gamenode/scheduler/include/mmo/game/sched/simulation_stage.h' \
  'server/gamenode/scheduler/include/mmo/game/sched/simulation_scheduler.h' \
  'server/gamenode/scheduler/include/mmo/game/sched/tick_timing.h' \
  'server/gamenode/scheduler/src/simulation_scheduler.cpp' \
  'server/gamenode/scheduler/tests/sched_sim_test.cpp' \
  'server/gamenode/scheduler/tests/sched_sim_bench.cpp' \
  'server/gamenode/scheduler/docs/INTERFACE.md' \
  'server/gamenode/scheduler/docs/PERFORMANCE.md'

# ---- 2b. 八阶段固定顺序体验证（grep 结构体验证）----
require_content 'server/gamenode/scheduler/include/mmo/game/sched/tick_phase.h' 'kTickPhaseOrder' '存在固定阶段顺序数组'
require_content 'server/gamenode/scheduler/include/mmo/game/sched/tick_phase.h' 'Replication'     '固定顺序含 Replication（末阶段）'
require_content 'server/gamenode/scheduler/include/mmo/game/sched/simulation_scheduler.h' 'max_catchup' 'Config 含 CatchUp 限幅'
require_content 'server/gamenode/scheduler/src/simulation_scheduler.cpp' 'CatchUpSteps' 'CatchUp 限幅实际生效'

# ---- 3. 模块边界：公开头不得 include 内部 src/ ----
if [ -d "$ROOT/server/gamenode/scheduler/include" ]; then
  scan_forbidden 'server/gamenode/scheduler/include' '#include\s+["<][^">]*src/[^">]*'
fi

# ---- 4. 编译（本地 MinGW + vcpkg，CI 不作为验收依据；Debug + Release 双构建）----
cmake_build_both

# ---- 5. 单元测试（ctest 过滤执行）----
run_ctest "$BUILD_TYPE" 'Sched_Sim' 'Sched_Sim'

# ---- 6. Benchmark 与性能阈值断言 ----
mkdir -p "$ROOT/bench"
run_bench "$BUILD_TYPE" 'bin/sched_sim_bench' --ticks 12000
assert_metric 'bench/sched_sim.txt' 'tick_overhead_ns' 'le' '20000'
assert_metric 'bench/sched_sim.txt' 'drift_us_per_10min' 'le' '50000'

# ---- 7. 验收结论 ----
info "人工复核项（脚本无法自动判定，必须人工确认后勾选）："
info "  [ ] 1. 八阶段固定顺序 Input→Movement→AOI→Combat→Buff→Quest→Event→Replication（TestRegisterOrder）"
info "  [ ] 2. 每阶段独立计时且 sum≈total（误差 <5%，TestPhaseTimingAccuracy）"
info "  [ ] 3. 长稳 12000 Tick（10min@20Hz）无累积漂移（TestLongStability）"
info "  [ ] 4. CatchUp 限幅 max_catchup=3，5s 空档只补 3 Tick（TestCatchUpCap）"
info "  [ ] 5. 单阶段异常不崩溃，后续阶段继续（TestStageExceptionSurvives / §19）"
info "  [ ] 6. 帧 Arena 每 Tick Reset，跨 Tick 不无界增长（TestFrameArenaReset）"
info "  [ ] 7. Tick 总耗时 >50ms 计 Overrun 且 Tick 继续（TestOverrunCount）"
info "  [ ] 8. Debug / Release 双构建通过，ctest -R Sched_Sim 全绿（8 测试函数 / 40+ 断言）"
info "  [ ] 9. 禁止在 Tick 内同步 IO / 禁止阻塞阶段（§10 人工确认）"
info "  [ ] 10. Start 后台线程自驱、Stop 完成当前 Tick 后退出不中途杀（TestStartStop 需另补，§19）"

end_task "TASK-013"
