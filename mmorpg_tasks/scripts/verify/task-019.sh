#!/usr/bin/env bash
# TASK-019 · Quest System —— 本地验收脚本
# 自动生成：python tools/gen/build_tasks.py   （禁止手工编辑，改数据源后重新生成）
# 环境：Windows Git Bash / MSYS2 MinGW；g++ (MinGW MSYS2)；vcpkg manifest mode baseline aae277ac
# 用法：bash scripts/verify/task-019.sh
#      BUILD_TYPE=Debug  bash scripts/verify/task-019.sh
# 红线：不做网络操作、不推送 Git、不写数据库；任一步失败即非零退出。

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
source "$ROOT/scripts/verify/_common.sh"

begin_task "TASK-019" 'Quest System'

# ---- 1. 前置任务门禁：必须全部 STATUS: DONE ----
require_tasks_done 007 016 018

# ---- 2. 交付物存在性 ----
require_files \
  'server/gamenode/quest/include/mmo/game/quest/quest_system.h'

# ---- 3. 模块边界：公开头不得 include 内部 src/ ----
if [ -d "$ROOT/server/gamenode/quest/include" ]; then
  scan_forbidden 'server/gamenode/quest/include' '#include\s+["<][^">]*src/[^">]*'
fi

# ---- 4. 编译（本地 MinGW + vcpkg，CI 不作为验收依据） ----
cmake_build_both

# ---- 5. 单元测试（ctest 过滤执行） ----
run_ctest "$BUILD_TYPE" 'Quest' 'Quest'

# ---- 6. Benchmark 与性能阈值断言 ----
mkdir -p "$ROOT/bench"
run_bench "$BUILD_TYPE" 'bin/quest_bench' --players 1000 --events 10000
assert_metric 'bench/quest.txt' 'event_handle_ns' 'le' '1000'
assert_metric 'bench/quest.txt' 'scaling_check_1k_vs_10k_players' 'le' '1.5'

# ---- 7. 验收结论 ----
info "人工复核项（脚本无法自动判定，必须人工确认后勾选）："
info "  [ ] 1. 四类事件（MonsterKilled/ItemCollected/NPCTalked/LocationReached）驱动进度全部实现"
info "  [ ] 2. **不存在每 Tick 遍历所有玩家的处理器**（反模式测试：耗时与玩家总数无关）"
info "  [ ] 3. 倒排索引：事件到达 O(1) 定位相关任务（benchmark 佐证）"
info "  [ ] 4. 交任务幂等，重复请求只发一次奖励（单测断言）"
info "  [ ] 5. 任务链前置校验生效"
info "  [ ] 6. 任务定义全部配置化（代码无硬编码）"
info "  [ ] 7. Debug / Release 双构建通过，ctest -R Quest 全绿"

end_task "TASK-019"
