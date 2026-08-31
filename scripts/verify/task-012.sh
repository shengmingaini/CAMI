#!/usr/bin/env bash
# TASK-012 · Scene System —— 本地验收脚本
# 自动生成：python tools/gen/build_tasks.py   （禁止手工编辑，改数据源后重新生成）
# 环境：Windows Git Bash / MSYS2 MinGW；g++ (MinGW MSYS2)；vcpkg manifest mode baseline aae277ac
# 用法：bash scripts/verify/task-012.sh
#      BUILD_TYPE=Debug  bash scripts/verify/task-012.sh
# 红线：不做网络操作、不推送 Git、不写数据库；任一步失败即非零退出。
#
# 注意：本仓库 build 缓存为 Ninja，必须注入 GENERATOR=Ninja（默认 MinGW Makefiles 会与
#       既有 Ninja 缓存冲突），并注入 MMO_PROJECT_ROOT / MMO_TASKS_DIR / VCPKG_ROOT / MinGW PATH。

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
source "$ROOT/scripts/verify/_common.sh"

begin_task "TASK-012" 'Scene System'

# ---- 1. 前置任务门禁：必须全部 STATUS: DONE ----
require_tasks_done 011

# ---- 2. 交付物存在性 ----
require_files \
  'server/gamenode/scene/include/mmo/game/scene/scene_id.h' \
  'server/gamenode/scene/include/mmo/game/scene/scene.h' \
  'server/gamenode/scene/include/mmo/game/scene/scene_context.h' \
  'server/gamenode/scene/include/mmo/game/scene/scene_manager.h' \
  'server/gamenode/scene/src/scene.cpp' \
  'server/gamenode/scene/src/scene_manager.cpp' \
  'server/gamenode/scene/tests/scene_test.cpp' \
  'server/gamenode/scene/tests/scene_bench.cpp' \
  'server/gamenode/scene/docs/INTERFACE.md' \
  'server/gamenode/scene/docs/PERFORMANCE.md'

# ---- 2b. Acceptance #2：Scene 必含五项（grep 结构体验证）----
require_content 'server/gamenode/scene/include/mmo/game/scene/scene.h' 'SceneID'       'Scene 含 SceneID'
require_content 'server/gamenode/scene/include/mmo/game/scene/scene.h' 'SceneVersion'  'Scene 含 SceneVersion'
require_content 'server/gamenode/scene/include/mmo/game/scene/scene.h' 'TickNumber'    'Scene 含 TickNumber'
require_content 'server/gamenode/scene/include/mmo/game/scene/scene.h' 'StateHash'     'Scene 含 StateHash'
require_content 'server/gamenode/scene/include/mmo/game/scene/scene.h' 'OwnerGameNode' 'Scene 含 OwnerGameNode'

# ---- 3. 模块边界：公开头不得 include 内部 src/ ----
if [ -d "$ROOT/server/gamenode/scene/include" ]; then
  scan_forbidden 'server/gamenode/scene/include' '#include\s+["<][^">]*src/[^">]*'
fi

# ---- 4. 编译（本地 MinGW + vcpkg，CI 不作为验收依据；Debug + Release 双构建）----
cmake_build_both

# ---- 5. 单元测试（ctest 过滤执行）----
run_ctest "$BUILD_TYPE" 'Scene' 'Game_Scene'

# ---- 6. Benchmark 与性能阈值断言 ----
mkdir -p "$ROOT/bench"
run_bench "$BUILD_TYPE" 'bin/scene_bench' --scenes 100 --ticks 1000
assert_metric 'bench/scene.txt' 'scene_tick_overhead_ns' 'le' '1000'
assert_metric 'bench/scene.txt' 'mem_bytes_per_scene' 'le' '65536'

# ---- 7. 验收结论 ----
info "人工复核项（脚本无法自动判定，必须人工确认后勾选）："
info "  [ ] 1. Scene 五状态机全路径有单测，非法转移返回错误（TestStateMachine）"
info "  [ ] 2. Enter/Leave 正确绑定/解绑 Avatar 实体并发布事件（TestEnterLeaveAndEvents）"
info "  [ ] 3. StateHash 相同操作序列可复现（确定性，TestStateHashDeterminism）"
info "  [ ] 4. 单 Scene 只能被一个线程 Tick（tick_mu_ 序列化，TestConcurrentTickAllNoReentry）"
info "  [ ] 5. 容量上限生效，超限返回 BUSY（TestCapacityBusy / TestDrainingRejectsEnter）"
info "  [ ] 6. Debug / Release 双构建通过，ctest -R Scene 全绿（9 测试函数 / 60+ 断言）"
info "  [ ] 7. 禁止系统反向持有 Scene 私有成员（只走 SceneContext，TestSceneContextReadOnly）"
info "  [ ] 8. 禁止无界 Scene 玩家/实体数量（players_ 有界 ≤ max_players_，§21）"
info "  [ ] 9. 禁止在 Tick 内同步销毁 Scene（Destroy 取快照于锁外，TickAll 持 tick_mu_）"
info "  [ ] 10. 禁止 StateHash 全量序列化（仅 FNV 滚动哈希关键状态，§21）"

end_task "TASK-012"
