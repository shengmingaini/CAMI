#!/usr/bin/env bash
# TASK-003 · Core Time / UUID / Config —— 本地验收脚本
# 自动生成：python tools/gen/build_tasks.py   （禁止手工编辑，改数据源后重新生成）
# 环境：Windows Git Bash / MSYS2 MinGW；g++ (MinGW MSYS2)；vcpkg manifest mode baseline aae277ac
# 用法：bash scripts/verify/task-003.sh
#      BUILD_TYPE=Debug  bash scripts/verify/task-003.sh
# 红线：不做网络操作、不推送 Git、不写数据库；任一步失败即非零退出。

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
source "$ROOT/scripts/verify/_common.sh"

begin_task "TASK-003" 'Core Time / UUID / Config'

# ---- 1. 前置任务门禁：必须全部 STATUS: DONE ----
require_tasks_done 000

# ---- 2. 交付物存在性 ----
require_files \
  'engine/core/include/mmo/core/time/tick_clock.h' \
  'config/tick.json' \
  'engine/core/include/mmo/core/time/clock.h' \
  'engine/core/include/mmo/core/uuid/uuid.h' \
  'engine/core/include/mmo/core/config/config_manager.h' \
  'config/app.json' \
  'config/network.json' \
  'engine/core/docs/INTERFACE.md'

# ---- 3. 静态红线扫描 ----
scan_forbidden 'engine/core/src/time' 'std::chrono::system_clock'
scan_forbidden 'engine/core/include/mmo/core/time' 'std::chrono::system_clock'

# ---- 4. 模块边界：公开头不得 include 内部 src/ ----
if [ -d "$ROOT/engine/core/include" ]; then
  scan_forbidden 'engine/core/include' '#include\s+["<][^">]*src/[^">]*'
fi

# ---- 5. 编译（本地 MinGW + vcpkg，CI 不作为验收依据） ----
cmake_build_both

# ---- 6. 单元测试（ctest 过滤执行） ----
run_ctest "$BUILD_TYPE" 'Core_Time' 'Core_Time'

# ---- 7. Benchmark 与性能阈值断言 ----
mkdir -p "$ROOT/bench"
run_bench "$BUILD_TYPE" 'bin/time_bench' --samples 1000000
assert_metric 'bench/core_time.txt' 'monotonic_ns_per_call' 'le' '25'
assert_metric 'bench/core_time.txt' 'config_get_ns' 'le' '50'

# ---- 8. 验收结论 ----
info "人工复核项（脚本无法自动判定，必须人工确认后勾选）："
info "  [ ] 1. 全仓 grep：Tick 相关代码路径中**不存在**对 WallClock / system_clock 的调用（红线扫描）"
info "  [ ] 2. TickClock 跑 10000 次，累计误差 < 10ms，无漂移"
info "  [ ] 3. UUID 100 万次无碰撞，V7 可按时间排序"
info "  [ ] 4. ConfigManager 热更期间并发读安全（TSan 或压测 100 万次读无异常）"
info "  [ ] 5. benchmark 三项指标达标"
info "  [ ] 6. config/ 下至少 3 份配置可被正确加载并 Get 到值"
info "  [ ] 7. Debug / Release 双构建通过，ctest -R Core_Time 全绿"

end_task "TASK-003"
