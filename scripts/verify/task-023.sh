#!/usr/bin/env bash
# TASK-023 · Buff / Debuff —— 本地验收脚本
# 自动生成：python tools/gen/build_tasks.py   （禁止手工编辑，改数据源后重新生成）
# 环境：Windows Git Bash / MSYS2 MinGW；g++ (MinGW MSYS2)；vcpkg manifest mode baseline aae277ac
# 用法：bash scripts/verify/task-023.sh
#      BUILD_TYPE=Debug  bash scripts/verify/task-023.sh
# 红线：不做网络操作、不推送 Git、不写数据库；任一步失败即非零退出。

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
source "$ROOT/scripts/verify/_common.sh"

begin_task "TASK-023" 'Buff / Debuff'

# ---- 1. 前置任务门禁：必须全部 STATUS: DONE ----
require_tasks_done 004 016

# ---- 2. 交付物存在性 ----
require_files \
  'server/gamenode/combat/include/mmo/game/combat/buff/buff_system.h' \
  'server/gamenode/combat/include/mmo/game/combat/buff/buff_def.h' \
  'server/gamenode/combat/docs/INTERFACE.md' \
  'server/gamenode/combat/docs/PERFORMANCE.md'

# ---- 3. 静态红线扫描 ----
scan_forbidden 'server/gamenode/combat/src/buff' 'std::thread'
scan_forbidden 'server/gamenode/combat/src/buff' '(mysql|redis|grpc)'

# ---- 4. 模块边界：公开头不得 include 内部 src/ ----
if [ -d "$ROOT/server/gamenode/combat/include" ]; then
  scan_forbidden 'server/gamenode/combat/include' '#include\s+["<][^">]*src/[^">]*'
fi

# ---- 5. 编译（本地 MinGW + vcpkg，CI 不作为验收依据） ----
cmake_build_both

# ---- 6. 单元测试（ctest 过滤执行） ----
run_ctest "$BUILD_TYPE" 'Buff' 'Buff'

# ---- 7. Benchmark 与性能阈值断言 ----
mkdir -p "$ROOT/bench"
run_bench "$BUILD_TYPE" 'bin/buff_bench' --entities 1000 --buffs-per-entity 20 --ticks 12000
assert_metric 'bench/buff.txt' 'buff_phase_us_at_20k' 'le' '400'
assert_metric 'bench/buff.txt' 'mem_bytes_per_buff' 'le' '64'
assert_metric 'bench/buff.txt' 'thread_count_delta' 'le' '0'

# ---- 8. 验收结论 ----
info "人工复核项（脚本无法自动判定，必须人工确认后勾选）："
info "  [ ] 1. **Buff Tick 走 Scheduler，不创建任何线程**（grep 验证 + 运行时 `thread_count` 不随 Buff 数增长）"
info "  [ ] 2. 三种堆叠规则全部实现且有单测"
info "  [ ] 3. 属性计算先加后乘，Buff 只写 from_buff 层（代码评审 + 单测）"
info "  [ ] 4. 2 万 Buff 同时存在，Buff 阶段耗时达标（benchmark 实测）"
info "  [ ] 5. DOT/HOT/护盾/控制四类 Buff 均可工作（集成测试）"
info "  [ ] 6. Buff 配置全部配置化"
info "  [ ] 7. Debug / Release 双构建通过，ctest -R Buff 全绿"

end_task "TASK-023"
