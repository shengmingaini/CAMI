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
  'engine/core/include/mmo/core/bus/command_bus.h' \
  'engine/core/tests/demo_pipeline.cpp' \
  'engine/core/include/mmo/core/bus/query_bus.h' \
  'engine/core/include/mmo/core/bus/event_bus.h' \
  'engine/core/docs/README.md' \
  'engine/core/docs/INTERFACE.md' \
  'engine/core/docs/PERFORMANCE.md'

# ---- 3. 静态红线扫描 ----
scan_forbidden 'engine/core/src/bus' 'std::thread'

# ---- 4. 模块边界：公开头不得 include 内部 src/ ----
if [ -d "$ROOT/engine/core/include" ]; then
  scan_forbidden 'engine/core/include' '#include\s+["<][^">]*src/[^">]*'
fi

# ---- 5. 编译（本地 MinGW + vcpkg，CI 不作为验收依据） ----
cmake_build_both

# ---- 6. 单元测试（ctest 过滤执行） ----
run_ctest "$BUILD_TYPE" 'Core_Bus' 'Core_Bus'

# ---- 7. Benchmark 与性能阈值断言 ----
mkdir -p "$ROOT/bench"
run_bench "$BUILD_TYPE" 'bin/bus_bench' --iterations 1000000
assert_metric 'bench/core_bus.txt' 'cmd_dispatch_ns' 'le' '150'
assert_metric 'bench/core_bus.txt' 'event_drain_ns_per_event' 'le' '80'

# ---- 8. 验收结论 ----
info "人工复核项（脚本无法自动判定，必须人工确认后勾选）："
info "  [ ] 1. Demo 链路跑通：Dispatch(Command) → Handler → Publish(Event) → Subscriber 收到（集成测试断言）"
info "  [ ] 2. Query 无副作用：测试替身可检测到任何写操作并判定失败"
info "  [ ] 3. EventBus **不创建线程**（grep 验证），Drain 由宿主驱动且带时间预算"
info "  [ ] 4. 队列满时：非关键事件丢弃并计数，关键事件返回 BUSY，行为可被单测断言"
info "  [ ] 5. 一个订阅者抛异常不影响其他订阅者（单测覆盖）"
info "  [ ] 6. 重复注册同一 Command 类型返回错误，不静默覆盖"
info "  [ ] 7. benchmark 指标达标并写入 docs/PERFORMANCE.md"
info "  [ ] 8. Debug / Release 双构建通过，ctest -R Core_Bus 全绿"

end_task "TASK-007"
