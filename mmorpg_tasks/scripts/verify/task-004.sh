#!/usr/bin/env bash
# TASK-004 · Core Memory / Thread / Scheduler —— 本地验收脚本
# 自动生成：python tools/gen/build_tasks.py   （禁止手工编辑，改数据源后重新生成）
# 环境：Windows Git Bash / MSYS2 MinGW；g++ (MinGW MSYS2)；vcpkg manifest mode baseline aae277ac
# 用法：bash scripts/verify/task-004.sh
#      BUILD_TYPE=Debug  bash scripts/verify/task-004.sh
# 红线：不做网络操作、不推送 Git、不写数据库；任一步失败即非零退出。

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
source "$ROOT/scripts/verify/_common.sh"

begin_task "TASK-004" 'Core Memory / Thread / Scheduler'

# ---- 1. 前置任务门禁：必须全部 STATUS: DONE ----
require_tasks_done 001 003

# ---- 2. 交付物存在性 ----
require_files \
  'engine/core/include/mmo/core/sched/scheduler.h' \
  'engine/core/include/mmo/core/memory/object_pool.h'

# ---- 3. 静态红线扫描 ----
scan_forbidden 'engine/core/src/sched' 'std::thread'
scan_forbidden 'engine/core/include/mmo/core/sched' 'std::thread'

# ---- 4. 模块边界：公开头不得 include 内部 src/ ----
if [ -d "$ROOT/engine/core/include" ]; then
  scan_forbidden 'engine/core/include' '#include\s+["<][^">]*src/[^">]*'
fi

# ---- 5. 编译（本地 MinGW + vcpkg，CI 不作为验收依据） ----
cmake_build_both

# ---- 6. 单元测试（ctest 过滤执行） ----
run_ctest "$BUILD_TYPE" 'Core_Thread' 'Core_Thread'

# ---- 7. Benchmark 与性能阈值断言 ----
mkdir -p "$ROOT/bench"
run_bench "$BUILD_TYPE" 'bin/sched_bench' --timers 10000 --ticks 1000
run_bench "$BUILD_TYPE" 'bin/mem_bench' --ops 10000000
assert_metric 'bench/core_sched.txt' 'sched_tick_us_10k_timers' 'le' '200'
assert_metric 'bench/core_mem.txt' 'pool_acquire_release_ns' 'le' '20'

# ---- 8. 验收结论 ----
info "人工复核项（脚本无法自动判定，必须人工确认后勾选）："
info "  [ ] 1. Scheduler **不创建任何线程**（grep 验证：`std::thread` 不出现在 sched/ 目录）"
info "  [ ] 2. 4×4 并发 10 万任务无丢失、无重复、无死锁"
info "  [ ] 3. Scheduler 挂 500 个 20Hz 周期任务，10 秒内触发次数误差 < 1%"
info "  [ ] 4. ObjectPool / MemoryPool / Arena 在 ASan 下无泄漏、无越界"
info "  [ ] 5. benchmark 四项指标达标并写入 docs/PERFORMANCE.md"
info "  [ ] 6. 每类线程暴露 Pending/ReadyCount 指标（供 TASK-039 指标采集）"
info "  [ ] 7. Debug / Release 双构建通过，ctest -R Core_Thread 全绿"

end_task "TASK-004"
