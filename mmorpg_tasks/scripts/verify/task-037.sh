#!/usr/bin/env bash
# TASK-037 · Reconnect / Failover / Scene Recovery —— 本地验收脚本
# 自动生成：python tools/gen/build_tasks.py   （禁止手工编辑，改数据源后重新生成）
# 环境：Windows Git Bash / MSYS2 MinGW；g++ (MinGW MSYS2)；vcpkg manifest mode baseline aae277ac
# 用法：bash scripts/verify/task-037.sh
#      BUILD_TYPE=Debug  bash scripts/verify/task-037.sh
# 红线：不做网络操作、不推送 Git、不写数据库；任一步失败即非零退出。

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
source "$ROOT/scripts/verify/_common.sh"

begin_task "TASK-037" 'Reconnect / Failover / Scene Recovery'

# ---- 1. 前置任务门禁：必须全部 STATUS: DONE ----
require_tasks_done 009 010 012 026 027

# ---- 2. 交付物存在性 ----
require_files \
  'server/gateway/include/mmo/gateway/resilience/failover_coordinator.h' \
  'docs/failover-drill-report.md' \
  'server/gateway/include/mmo/gateway/resilience/health_monitor.h' \
  'server/gateway/include/mmo/gateway/resilience/reconnect_service.h' \
  'server/gamenode/scene/include/mmo/game/scene/recovery/scene_recovery.h' \
  'tools/chaos/kill_gamenode.sh'

# ---- 3. 模块边界：公开头不得 include 内部 src/ ----
if [ -d "$ROOT/server/gateway + server/gamenode/include" ]; then
  scan_forbidden 'server/gateway + server/gamenode/include' '#include\s+["<][^">]*src/[^">]*'
fi

# ---- 4. 编译（本地 MinGW + vcpkg，CI 不作为验收依据） ----
cmake_build_both

# ---- 5. 单元测试（ctest 过滤执行） ----
run_ctest "$BUILD_TYPE" 'Resilience' 'Resilience'

# ---- 6. Benchmark 与性能阈值断言 ----
mkdir -p "$ROOT/bench"
run_bench "$BUILD_TYPE" 'bin/resilience_bench' --players 1000
assert_metric 'bench/resilience.txt' 'detect_ms' 'le' '15000'
assert_metric 'bench/resilience.txt' 'reattach_ms_per_player' 'le' '500'

# ---- 7. 验收结论 ----
info "人工复核项（脚本无法自动判定，必须人工确认后勾选）："
info "  [ ] 1. **37.1 / 37.2 / 37.3 / 37.4 四个子项全部实现**，各有独立测试"
info "  [ ] 2. 端到端演练四项全部跑通（见上表，需真实 kill 进程）"
info "  [ ] 3. GameNode 崩溃后 15 秒内检测 + 玩家可继续游戏"
info "  [ ] 4. version 校验拒绝旧连接回放（单测）"
info "  [ ] 5. 恢复后无数据损坏：无重复物品、无负余额、无属性错乱（对账校验）"
info "  [ ] 6. 批量重连限速生效，不雪崩"
info "  [ ] 7. **明确记录**：第一版不做 Live Scene Migration，恢复可能回退到最近 Checkpoint（写入文档）"
info "  [ ] 8. Debug / Release 双构建通过，ctest -R Resilience 全绿"

end_task "TASK-037"
