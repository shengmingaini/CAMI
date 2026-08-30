#!/usr/bin/env bash
# TASK-007 · Command / Query / Event Bus —— 本地验收脚本
# 自动生成：python tools/gen/build_tasks.py   （禁止手工编辑，改数据源后重新生成）
# 环境：Windows Git Bash / MSYS2 MinGW；g++ (MinGW MSYS2)；vcpkg manifest mode baseline aae277ac
# 用法：bash scripts/verify/task-007.sh
#      BUILD_TYPE=Debug  bash scripts/verify/task-007.sh
# 红线：不做网络操作、不推送 Git、不写数据库；任一步失败即非零退出。

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
source "$ROOT/scripts/verify/_common.sh"

begin_task "TASK-007" 'Command / Query / Event Bus'

# ---- 1. 前置任务门禁：必须全部 STATUS: DONE ----
require_tasks_done 001 004 005

# ---- 2. 交付物存在性 ----
require_files \
  'engine/core/include/mmo/core/bus/event_bus.h' \
  'engine/core/docs/INTERFACE.md'

# ---- 3. 模块边界：公开头不得 include 内部 src/ ----
if [ -d "$ROOT/engine/core/include" ]; then
  scan_forbidden 'engine/core/include' '#include\s+["<][^">]*src/[^">]*'
fi

# ---- 4. 红线：EventBus 禁止自带线程（必须宿主 Drain） ----
scan_forbidden 'engine/core/src/bus' 'std::thread'

# ---- 5. 编译（本地 MinGW + vcpkg，CI 不作为验收依据） ----
cmake_build_both

# ---- 6. 单元测试（ctest 过滤执行：含 Suite + Demo） ----
run_ctest "$BUILD_TYPE" 'Core_Bus' 'Core_Bus'

# ---- 7. Benchmark 与性能阈值断言 ----
mkdir -p "$ROOT/bench"
run_bench "$BUILD_TYPE" 'bin/bus_bench' --iterations 1000000
assert_metric 'bench/core_bus.txt' 'cmd_dispatch_ns' 'le' '150'
assert_metric 'bench/core_bus.txt' 'event_drain_ns_per_event' 'le' '80'
assert_metric 'bench/core_bus.txt' 'alloc_per_cmd' 'le' '0'

# ---- 8. 验收结论 ----
info "人工复核项（脚本无法自动判定，必须人工确认后勾选）："
info "  [ ] 1. Demo 链路跑通：Command → Handler → Event → 2 个 Subscriber（Core_Bus.Demo）"
info "  [ ] 2. Query 无副作用：SideEffectProbe 可捕获只读区间内的写入（bus_test 断言）"
info "  [ ] 3. 重复注册同一 Command 返回错误，未静默覆盖（单测断言仍执行第一个 handler）"
info "  [ ] 4. 队列满时非关键事件丢弃计数、关键事件返回 BUSY（单测断言 DroppedCount 不变）"
info "  [ ] 5. 一个订阅者抛异常不影响其他订阅者（SubscriberErrors 计数 + 后续照常收到）"
info "  [ ] 6. EventBus 不创建线程，Drain 由宿主驱动且带时间预算（脚本已 grep 验证）"
info "  [ ] 7. benchmark 四项指标写入 docs/PERFORMANCE.md"
info "  [ ] 8. Debug / Release 双构建通过，ctest -R Core_Bus 全绿"

end_task "TASK-007"
