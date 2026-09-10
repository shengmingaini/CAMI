#!/usr/bin/env bash
# TASK-011 · Entity System —— 本地验收脚本
# 自动生成：python tools/gen/build_tasks.py   （禁止手工编辑，改数据源后重新生成）
# 环境：Windows Git Bash / MSYS2 MinGW；g++ (MinGW MSYS2)；vcpkg manifest mode baseline aae277ac
# 用法：bash scripts/verify/task-011.sh
#      BUILD_TYPE=Debug  bash scripts/verify/task-011.sh
# 红线：不做网络操作、不推送 Git、不写数据库；任一步失败即非零退出。

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
source "$ROOT/scripts/verify/_common.sh"

begin_task "TASK-011" 'Entity System'

# ---- 1. 前置任务门禁：必须全部 STATUS: DONE ----
require_tasks_done 004 007

# ---- 2. 交付物存在性 ----
require_files \
  'server/gamenode/entity/include/mmo/game/entity/entity_manager.h' \
  'server/gamenode/entity/include/mmo/game/entity/entity.h' \
  'server/gamenode/entity/include/mmo/game/entity/component_store.h' \
  'server/gamenode/entity/docs/INTERFACE.md' \
  'server/gamenode/entity/docs/PERFORMANCE.md'

# ---- 3. 模块边界：公开头不得 include 内部 src/ ----
if [ -d "$ROOT/server/gamenode/entity/include" ]; then
  scan_forbidden 'server/gamenode/entity/include' '#include\s+["<][^">]*src/[^">]*'
fi

# ---- 4. 编译（本地 MinGW + vcpkg，CI 不作为验收依据） ----
cmake_build_both

# ---- 5. 单元测试（ctest 过滤执行） ----
run_ctest "$BUILD_TYPE" 'Entity' 'Entity'

# ---- 6. Benchmark 与性能阈值断言 ----
mkdir -p "$ROOT/bench"
run_bench "$BUILD_TYPE" 'bin/entity_bench' --entities 100000
assert_metric 'bench/entity.txt' 'mem_bytes_per_entity' 'le' '256'
assert_metric 'bench/entity.txt' 'create_ns_per_entity' 'le' '100'

# ---- 7. 验收结论 ----
info "人工复核项（脚本无法自动判定，必须人工确认后勾选）："
info "  [ ] 1. EntityId 含 generation，销毁后旧 Id 查找返回 nullptr（防 ABA，单测）"
info "  [ ] 2. Create / Destroy / Find 均为 O(1)（benchmark 佐证：10 万实体下耗时线性且常数因子达标）"
info "  [ ] 3. 延迟销毁在 Tick 边界统一执行，ASan 下无悬垂访问"
info "  [ ] 4. 组件增删查类型安全，错误类型返回 nullptr"
info "  [ ] 5. `mem_bytes_per_entity` 达标（目标见性能期望）"
info "  [ ] 6. 全部生命周期事件可通过 EventBus 观测（集成测试订阅断言）"
info "  [ ] 7. Debug / Release 双构建通过，ctest -R Entity 全绿"

end_task "TASK-011"
