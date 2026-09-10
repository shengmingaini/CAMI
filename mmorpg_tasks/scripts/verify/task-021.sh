#!/usr/bin/env bash
# TASK-021 · Skill System —— 本地验收脚本
# 自动生成：python tools/gen/build_tasks.py   （禁止手工编辑，改数据源后重新生成）
# 环境：Windows Git Bash / MSYS2 MinGW；g++ (MinGW MSYS2)；vcpkg manifest mode baseline aae277ac
# 用法：bash scripts/verify/task-021.sh
#      BUILD_TYPE=Debug  bash scripts/verify/task-021.sh
# 红线：不做网络操作、不推送 Git、不写数据库；任一步失败即非零退出。

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
source "$ROOT/scripts/verify/_common.sh"

begin_task "TASK-021" 'Skill System'

# ---- 1. 前置任务门禁：必须全部 STATUS: DONE ----
require_tasks_done 011 016

# ---- 2. 交付物存在性 ----
require_files \
  'server/gamenode/combat/include/mmo/game/combat/skill/skill_system.h' \
  'server/gamenode/combat/include/mmo/game/combat/skill/skill_def.h' \
  'server/gamenode/combat/docs/INTERFACE.md'

# ---- 3. 模块边界：公开头不得 include 内部 src/ ----
if [ -d "$ROOT/server/gamenode/combat/include" ]; then
  scan_forbidden 'server/gamenode/combat/include' '#include\s+["<][^">]*src/[^">]*'
fi

# ---- 4. 编译（本地 MinGW + vcpkg，CI 不作为验收依据） ----
cmake_build_both

# ---- 5. 单元测试（ctest 过滤执行） ----
run_ctest "$BUILD_TYPE" 'Skill' 'Skill'

# ---- 6. Benchmark 与性能阈值断言 ----
mkdir -p "$ROOT/bench"
run_bench "$BUILD_TYPE" 'bin/skill_bench' --casts 100000
assert_metric 'bench/skill.txt' 'try_cast_ns' 'le' '1000'
assert_metric 'bench/skill.txt' 'cooldown_query_ns' 'le' '20'

# ---- 7. 验收结论 ----
info "人工复核项（脚本无法自动判定，必须人工确认后勾选）："
info "  [ ] 1. 四类目标（Self / SingleTarget / AOE / Projectile）全部实现且有测试"
info "  [ ] 2. 冷却、消耗、距离、目标四类校验齐全，CastResult 各分支可达"
info "  [ ] 3. 读条可被中断且中断不退资源（单测断言）"
info "  [ ] 4. AOE 目标选取走 AOI 局部查询（grep + benchmark 佐证）"
info "  [ ] 5. 客户端重复发包不会重复结算（冷却拦截，集成测试）"
info "  [ ] 6. 技能配置全部配置化（代码无硬编码数值）"
info "  [ ] 7. Debug / Release 双构建通过，ctest -R Skill 全绿"

end_task "TASK-021"
