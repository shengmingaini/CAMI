#!/usr/bin/env bash
# TASK-017 · Inventory / Equipment —— 本地验收脚本
# 自动生成：python tools/gen/build_tasks.py   （禁止手工编辑，改数据源后重新生成）
# 环境：Windows Git Bash / MSYS2 MinGW；g++ (MinGW MSYS2)；vcpkg manifest mode baseline aae277ac
# 用法：bash scripts/verify/task-017.sh
#      BUILD_TYPE=Debug  bash scripts/verify/task-017.sh
# 红线：不做网络操作、不推送 Git、不写数据库；任一步失败即非零退出。

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
source "$ROOT/scripts/verify/_common.sh"

begin_task "TASK-017" 'Inventory / Equipment'

# ---- 1. 前置任务门禁：必须全部 STATUS: DONE ----
require_tasks_done 007 016

# ---- 2. 交付物存在性 ----
require_files \
  'server/gamenode/inventory/include/mmo/game/inventory/inventory_system.h' \
  'server/gamenode/inventory/include/mmo/game/inventory/item.h' \
  'server/gamenode/inventory/include/mmo/game/inventory/inventory.h' \
  'server/gamenode/inventory/docs/INTERFACE.md' \
  'server/gamenode/inventory/docs/README.md'

# ---- 3. 模块边界：公开头不得 include 内部 src/ ----
if [ -d "$ROOT/server/gamenode/inventory/include" ]; then
  scan_forbidden 'server/gamenode/inventory/include' '#include\s+["<][^">]*src/[^">]*'
fi

# ---- 4. 编译（本地 MinGW + vcpkg，CI 不作为验收依据） ----
cmake_build_both

# ---- 5. 单元测试（ctest 过滤执行） ----
run_ctest "$BUILD_TYPE" 'Inventory' 'Inventory'

# ---- 6. Benchmark 与性能阈值断言 ----
mkdir -p "$ROOT/bench"
run_bench "$BUILD_TYPE" 'bin/inventory_bench' --players 1000 --ops 100
assert_metric 'bench/inventory.txt' 'equip_ns' 'le' '2000'
assert_metric 'bench/inventory.txt' 'mem_bytes_per_item' 'le' '64'

# ---- 7. 验收结论 ----
info "人工复核项（脚本无法自动判定，必须人工确认后勾选）："
info "  [ ] 1. **四种操作全部产生标准 Command + Event**（集成测试断言事件数 == 操作数）"
info "  [ ] 2. 装备类物品 ItemGuid 全局唯一（100 万次无重复，防复制）"
info "  [ ] 3. 装备/卸下正确触发属性重算（from_equipment 层）"
info "  [ ] 4. 背包满、物品不足、等级不足、槽位不匹配等边界全部有测试且行为明确"
info "  [ ] 5. 无物品复制、无物品丢失（1000 玩家 × 100 次随机操作守恒校验）"
info "  [ ] 6. 配置化物品表，代码无硬编码物品属性"
info "  [ ] 7. Debug / Release 双构建通过，ctest -R Inventory 全绿"

end_task "TASK-017"
