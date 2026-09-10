#!/usr/bin/env bash
# TASK-012 · Scene System —— 本地验收脚本
# 自动生成：python tools/gen/build_tasks.py   （禁止手工编辑，改数据源后重新生成）
# 环境：Windows Git Bash / MSYS2 MinGW；g++ (MinGW MSYS2)；vcpkg manifest mode baseline aae277ac
# 用法：bash scripts/verify/task-012.sh
#      BUILD_TYPE=Debug  bash scripts/verify/task-012.sh
# 红线：不做网络操作、不推送 Git、不写数据库；任一步失败即非零退出。

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
source "$ROOT/scripts/verify/_common.sh"

begin_task "TASK-012" 'Scene System'

# ---- 1. 前置任务门禁：必须全部 STATUS: DONE ----
require_tasks_done 011

# ---- 2. 交付物存在性 ----
require_files \
  'server/gamenode/scene/include/mmo/game/scene/scene.h' \
  'server/gamenode/scene/include/mmo/game/scene/scene_context.h' \
  'server/gamenode/scene/include/mmo/game/scene/scene_manager.h' \
  'server/gamenode/scene/docs/INTERFACE.md' \
  'server/gamenode/scene/docs/README.md'

# ---- 3. 模块边界：公开头不得 include 内部 src/ ----
if [ -d "$ROOT/server/gamenode/scene/include" ]; then
  scan_forbidden 'server/gamenode/scene/include' '#include\s+["<][^">]*src/[^">]*'
fi

# ---- 4. 编译（本地 MinGW + vcpkg，CI 不作为验收依据） ----
cmake_build_both

# ---- 5. 单元测试（ctest 过滤执行） ----
run_ctest "$BUILD_TYPE" 'Scene' 'Scene'

# ---- 6. Benchmark 与性能阈值断言 ----
mkdir -p "$ROOT/bench"
run_bench "$BUILD_TYPE" 'bin/scene_bench' --scenes 100 --ticks 1000
assert_metric 'bench/scene.txt' 'scene_tick_overhead_ns' 'le' '1000'
assert_metric 'bench/scene.txt' 'mem_bytes_per_scene' 'le' '65536'

# ---- 7. 验收结论 ----
info "人工复核项（脚本无法自动判定，必须人工确认后勾选）："
info "  [ ] 1. Scene 五状态机全部路径有单测，非法转移返回错误"
info "  [ ] 2. Scene 必含 SceneID / SceneVersion / TickNumber / StateHash / OwnerGameNode 五项（grep 结构体验证）"
info "  [ ] 3. Enter/Leave 正确创建/销毁 Avatar 实体并发布事件"
info "  [ ] 4. StateHash 在相同操作序列下可复现（确定性，单测断言两次结果一致）"
info "  [ ] 5. 单 Scene 只能被一个线程 Tick（代码评审 + 并发测试）"
info "  [ ] 6. 容量上限生效，超限返回 BUSY"
info "  [ ] 7. Debug / Release 双构建通过，ctest -R Scene 全绿"

end_task "TASK-012"
