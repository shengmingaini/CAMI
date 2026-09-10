#!/usr/bin/env bash
# TASK-010 · Gateway Router —— 本地验收脚本
# 自动生成：python tools/gen/build_tasks.py   （禁止手工编辑，改数据源后重新生成）
# 环境：Windows Git Bash / MSYS2 MinGW；g++ (MinGW MSYS2)；vcpkg manifest mode baseline aae277ac
# 用法：bash scripts/verify/task-010.sh
#      BUILD_TYPE=Debug  bash scripts/verify/task-010.sh
# 红线：不做网络操作、不推送 Git、不写数据库；任一步失败即非零退出。

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
source "$ROOT/scripts/verify/_common.sh"

begin_task "TASK-010" 'Gateway Router'

# ---- 1. 前置任务门禁：必须全部 STATUS: DONE ----
require_tasks_done 007 009

# ---- 2. 交付物存在性 ----
require_files \
  'server/gateway/include/mmo/gateway/route/player_router.h' \
  'server/gateway/include/mmo/gateway/route/node_registry.h' \
  'server/gateway/include/mmo/gateway/route/scene_router.h' \
  'server/gateway/include/mmo/gateway/route/route_cache.h' \
  'server/gateway/docs/INTERFACE.md' \
  'server/gateway/docs/PERFORMANCE.md'

# ---- 3. 模块边界：公开头不得 include 内部 src/ ----
if [ -d "$ROOT/server/gateway/include" ]; then
  scan_forbidden 'server/gateway/include' '#include\s+["<][^">]*src/[^">]*'
fi

# ---- 4. 编译（本地 MinGW + vcpkg，CI 不作为验收依据） ----
cmake_build_both

# ---- 5. 单元测试（ctest 过滤执行） ----
run_ctest "$BUILD_TYPE" 'Gateway_Route' 'Gateway_Route'

# ---- 6. Benchmark 与性能阈值断言 ----
mkdir -p "$ROOT/bench"
run_bench "$BUILD_TYPE" 'bin/route_bench' --lookups 1000000
assert_metric 'bench/gateway_route.txt' 'route_lookup_ns' 'le' '100'
assert_metric 'bench/gateway_route.txt' 'cache_hit_rate' 'ge' '0.99'

# ---- 7. 验收结论 ----
info "人工复核项（脚本无法自动判定，必须人工确认后勾选）："
info "  [ ] 1. **全链路跑通**：Login → Gateway → GameNode → Scene，集成测试断言 Session 的 GameNodeID 与 SceneID 被正确写入"
info "  [ ] 2. PlayerRouter 与 SceneRouter 均实现，Scene Owner 唯一性有测试保障"
info "  [ ] 3. RouteCache 有界（LRU），命中率 > 99%（benchmark 实测）"
info "  [ ] 4. 节点 Dead 后缓存批量失效，请求不再打向死节点（集成测试）"
info "  [ ] 5. 全部节点不可用时返回 BUSY 而非崩溃"
info "  [ ] 6. 路由查询 < 100ns（benchmark 实测）"
info "  [ ] 7. Debug / Release 双构建通过，ctest -R Gateway_Route 全绿"

end_task "TASK-010"
