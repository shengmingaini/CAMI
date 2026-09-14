#!/usr/bin/env bash
# TASK-022 · Damage / Heal —— 本地验收脚本
# 自动生成：python tools/gen/build_tasks.py   （禁止手工编辑，改数据源后重新生成）
# 环境：Windows Git Bash / MSYS2 MinGW；g++ (MinGW MSYS2)；vcpkg manifest mode baseline aae277ac
# 用法：bash scripts/verify/task-022.sh
#      BUILD_TYPE=Debug  bash scripts/verify/task-022.sh
# 红线：不做网络操作、不推送 Git、不写数据库；任一步失败即非零退出。

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
source "$ROOT/scripts/verify/_common.sh"

begin_task "TASK-022" 'Damage / Heal'

# ---- 1. 前置任务门禁：必须全部 STATUS: DONE ----
require_tasks_done 011 016 021

# ---- 2. 交付物存在性 ----
require_files \
  'server/gamenode/combat/include/mmo/game/combat/damage/damage_system.h' \
  'server/gamenode/combat/include/mmo/game/combat/damage/damage.h' \
  'server/gamenode/combat/include/mmo/game/combat/damage/prng.h' \
  'config/gameplay/combat/formula.json' \
  'server/gamenode/combat/docs/INTERFACE.md'

# ---- 3. 静态红线扫描 ----
scan_forbidden 'server/gamenode/combat/src/damage' '(mysql|redis|grpc|kafka|sql::|std::ifstream|std::ofstream)'

# ---- 4. 模块边界：公开头不得 include 内部 src/ ----
if [ -d "$ROOT/server/gamenode/combat/include" ]; then
  scan_forbidden 'server/gamenode/combat/include' '#include\s+["<][^">]*src/[^">]*'
fi

# ---- 5. 编译（本地 MinGW + vcpkg，CI 不作为验收依据） ----
cmake_build_both

# ---- 6. 单元测试（ctest 过滤执行） ----
run_ctest "$BUILD_TYPE" 'Damage' 'Damage'

# ---- 7. Benchmark 与性能阈值断言 ----
mkdir -p "$ROOT/bench"
run_bench "$BUILD_TYPE" 'bin/damage_bench' --iterations 1000000
assert_metric 'bench/damage.txt' 'compute_damage_ns' 'le' '50'
assert_metric 'bench/damage.txt' 'alloc_per_damage' 'le' '0'

# ---- 8. 验收结论 ----
info "人工复核项（脚本无法自动判定，必须人工确认后勾选）："
info "  [ ] 1. **伤害公式全部参数配置化**（grep 代码无硬编码系数）"
info "  [ ] 2. 结算顺序固定：暴击 → 闪避 → 抗性 → 护盾 → 扣血 → 致死 → 事件（单测断言顺序）"
info "  [ ] 3. **战斗结算不引用 MySQL / Redis / Kafka / gRPC**（链接符号检查通过）"
info "  [ ] 4. 确定性 PRNG：同种子产生同结果（单测）"
info "  [ ] 5. HP 守恒，无负值、无溢出（10000 次结算校验）"
info "  [ ] 6. 采样日志生效，不每条全写（日志量可测）"
info "  [ ] 7. Debug / Release 双构建通过，ctest -R Damage 全绿"

end_task "TASK-022"
