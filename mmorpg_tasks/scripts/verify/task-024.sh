#!/usr/bin/env bash
# TASK-024 · Combat Framework —— 本地验收脚本
# 自动生成：python tools/gen/build_tasks.py   （禁止手工编辑，改数据源后重新生成）
# 环境：Windows Git Bash / MSYS2 MinGW；g++ (MinGW MSYS2)；vcpkg manifest mode baseline aae277ac
# 用法：bash scripts/verify/task-024.sh
#      BUILD_TYPE=Debug  bash scripts/verify/task-024.sh
# 红线：不做网络操作、不推送 Git、不写数据库；任一步失败即非零退出。

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
source "$ROOT/scripts/verify/_common.sh"

begin_task "TASK-024" 'Combat Framework'

# ---- 1. 前置任务门禁：必须全部 STATUS: DONE ----
require_tasks_done 021 022 023 018

# ---- 2. 交付物存在性 ----
require_files \
  'server/gamenode/combat/include/mmo/game/combat/combat_system.h'

# ---- 3. 静态红线扫描 ----
scan_forbidden 'server/gamenode/combat/src' '(mysql|redis|grpc|kafka|sql::)'

# ---- 4. 模块边界：公开头不得 include 内部 src/ ----
if [ -d "$ROOT/server/gamenode/combat/include" ]; then
  scan_forbidden 'server/gamenode/combat/include' '#include\s+["<][^">]*src/[^">]*'
fi

# ---- 5. 编译（本地 MinGW + vcpkg，CI 不作为验收依据） ----
cmake_build_both

# ---- 6. 单元测试（ctest 过滤执行） ----
run_ctest "$BUILD_TYPE" 'Combat' 'Combat'

# ---- 7. Benchmark 与性能阈值断言 ----
mkdir -p "$ROOT/bench"
run_bench "$BUILD_TYPE" 'bin/combat_bench' --entities 1000 --combat-ratio 0.5
assert_metric 'bench/combat.txt' 'combat_phase_us_at_1k' 'le' '1200'
assert_metric 'bench/combat.txt' 'alloc_per_combat_tick' 'le' '0'

# ---- 8. 验收结论 ----
info "人工复核项（脚本无法自动判定，必须人工确认后勾选）："
info "  [ ] 1. Skill / Damage / Buff / Threat / Target 全部整合进 CombatSystem"
info "  [ ] 2. 仇恨表定长 16，溢出淘汰最低威胁（单测）"
info "  [ ] 3. **Combat 阶段不引用 MySQL / Redis / Kafka / 同步 gRPC**（链接符号 + 红线扫描）"
info "  [ ] 4. 控制类 Buff 正确影响移动与施法（集成测试）"
info "  [ ] 5. 完整战斗流程集成测试通过（含嘲讽、治疗、DOT、死亡、脱战）"
info "  [ ] 6. 战斗状态用位标记（grep 无散落 bool 战斗状态判断）"
info "  [ ] 7. Debug / Release 双构建通过，ctest -R Combat 全绿"

end_task "TASK-024"
