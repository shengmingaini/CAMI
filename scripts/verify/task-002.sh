#!/usr/bin/env bash
# TASK-002 · Core Logger / Trace —— 本地验收脚本
# 自动生成：python tools/gen/build_tasks.py   （禁止手工编辑，改数据源后重新生成）
# 环境：Windows Git Bash / MSYS2 MinGW；g++ (MinGW MSYS2)；vcpkg manifest mode baseline aae277ac
# 用法：bash scripts/verify/task-002.sh
#      BUILD_TYPE=Debug  bash scripts/verify/task-002.sh
# 红线：不做网络操作、不推送 Git、不写数据库；任一步失败即非零退出。

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
source "$ROOT/scripts/verify/_common.sh"

begin_task "TASK-002" 'Core Logger / Trace'

# ---- 1. 前置任务门禁：必须全部 STATUS: DONE ----
require_tasks_done 001

# ---- 2. 交付物存在性 ----
require_files \
  'engine/core/include/mmo/core/log/logger.h' \
  'tools/logtrace/parse_trace.py'

# ---- 3. 静态红线扫描 ----
scan_forbidden 'engine/core/src' '\bstd::cout\s*<<'
scan_forbidden 'engine/core/src' '\bprintf\s*\('

# ---- 4. 模块边界：公开头不得 include 内部 src/ ----
if [ -d "$ROOT/engine/core/include" ]; then
  scan_forbidden 'engine/core/include' '#include\s+["<][^">]*src/[^">]*'
fi

# ---- 5. 编译（本地 MinGW + vcpkg，CI 不作为验收依据） ----
cmake_build_both

# ---- 6. 单元测试（ctest 过滤执行） ----
run_ctest "$BUILD_TYPE" 'Core_Log' 'Core_Log'

# ---- 7. Benchmark 与性能阈值断言 ----
mkdir -p "$ROOT/bench"
run_bench "$BUILD_TYPE" 'bin/log_bench' --threads 8 --per-thread 100000
assert_metric 'bench/core_log.txt' 'disabled_ns_per_call' 'lt' '5'
assert_metric 'bench/core_log.txt' 'log_ns_per_msg' 'le' '800'

# ---- 8. 验收结论 ----
info "人工复核项（脚本无法自动判定，必须人工确认后勾选）："
info "  [ ] 1. 九项字段全部出现在每条日志记录中（JSON 模式下 key 齐全，单测断言）"
info "  [ ] 2. 8 线程 × 10 万条压测无死锁、无交叉错乱，单线程内顺序严格递增"
info "  [ ] 3. 给定 TraceID，`python tools/logtrace/parse_trace.py <trace>` 能完整串起跨线程日志"
info "  [ ] 4. 关闭日志时单次 `MMO_LOG` 调用开销 < 5ns（benchmark 实测）"
info "  [ ] 5. 环形队列满时 `dropped` 计数正确且业务线程不阻塞"
info "  [ ] 6. 全仓 grep 无 `std::cout` / `printf` 直接输出（红线扫描）"
info "  [ ] 7. Debug / Release 双构建通过，ctest -R Core_Log 全绿"

end_task "TASK-002"
