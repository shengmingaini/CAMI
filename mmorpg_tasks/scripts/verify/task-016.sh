#!/usr/bin/env bash
# TASK-016 · Player / Character —— 本地验收脚本
# 自动生成：python tools/gen/build_tasks.py   （禁止手工编辑，改数据源后重新生成）
# 环境：Windows Git Bash / MSYS2 MinGW；g++ (MinGW MSYS2)；vcpkg manifest mode baseline aae277ac
# 用法：bash scripts/verify/task-016.sh
#      BUILD_TYPE=Debug  bash scripts/verify/task-016.sh
# 红线：不做网络操作、不推送 Git、不写数据库；任一步失败即非零退出。

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
source "$ROOT/scripts/verify/_common.sh"

begin_task "TASK-016" 'Player / Character'

# ---- 1. 前置任务门禁：必须全部 STATUS: DONE ----
require_tasks_done 011 012 015

# ---- 2. 交付物存在性 ----
require_files \
  'server/gamenode/role/include/mmo/game/role/attribute.h' \
  'server/gamenode/role/include/mmo/game/role/character.h' \
  'server/gamenode/role/include/mmo/game/role/role_system.h' \
  'config/gameplay/exp_curve.json' \
  'server/gamenode/role/docs/INTERFACE.md' \
  'server/gamenode/role/docs/README.md'

# ---- 3. 静态红线扫描 ----
scan_forbidden 'server/gamenode/role/src' '(mysql|redis|grpc|sql::)'

# ---- 4. 模块边界：公开头不得 include 内部 src/ ----
if [ -d "$ROOT/server/gamenode/role/include" ]; then
  scan_forbidden 'server/gamenode/role/include' '#include\s+["<][^">]*src/[^">]*'
fi

# ---- 5. 编译（本地 MinGW + vcpkg，CI 不作为验收依据） ----
cmake_build_both

# ---- 6. 单元测试（ctest 过滤执行） ----
run_ctest "$BUILD_TYPE" 'Role' 'Role'

# ---- 7. Benchmark 与性能阈值断言 ----
mkdir -p "$ROOT/bench"
run_bench "$BUILD_TYPE" 'bin/role_bench' --characters 1000
assert_metric 'bench/role.txt' 'mem_bytes_per_character' 'le' '512'
assert_metric 'bench/role.txt' 'recompute_ns' 'le' '300'

# ---- 8. 验收结论 ----
info "人工复核项（脚本无法自动判定，必须人工确认后勾选）："
info "  [ ] 1. 属性三层模型（base/equipment/buff）实现，各系统不能直接改 Final 值（代码评审）"
info "  [ ] 2. 经验曲线配置化，代码中无硬编码数值（grep 验证）"
info "  [ ] 3. **Role 模块不访问 MySQL / Redis / 网络**（红线扫描，只走 PersistenceAdapter 接口）"
info "  [ ] 4. HP/MP 边界钳制与死亡事件正确（单测）"
info "  [ ] 5. 存档异步，不阻塞 Tick（集成测试测量 Tick P99）"
info "  [ ] 6. 1000 角色场景内存占用达标（benchmark）"
info "  [ ] 7. Debug / Release 双构建通过，ctest -R Role 全绿"

end_task "TASK-016"
