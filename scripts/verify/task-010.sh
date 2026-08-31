#!/usr/bin/env bash
# TASK-010 · Gateway Router —— 本地验收脚本
# 自动生成：python tools/gen/build_tasks.py   （禁止手工编辑，改数据源后重新生成）
# 环境：Windows Git Bash / MSYS2 MinGW；g++ (MinGW MSYS2)；vcpkg manifest mode baseline aae277ac
# 用法：bash scripts/verify/task-010.sh
#      BUILD_TYPE=Debug  bash scripts/verify/task-010.sh
# 红线：不做网络操作、不推送 Git、不写数据库；任一步失败即非零退出。
#
# 注意：本仓库 build 缓存为 Ninja，必须注入 GENERATOR=Ninja（默认 MinGW Makefiles 会与
#       既有 Ninja 缓存冲突），并注入 MMO_PROJECT_ROOT / MMO_TASKS_DIR / VCPKG_ROOT / MinGW PATH。

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
source "$ROOT/scripts/verify/_common.sh"

begin_task "TASK-010" 'Gateway Router'

# ---- 1. 前置任务门禁：必须全部 STATUS: DONE ----
require_tasks_done 007 009

# ---- 2. 交付物存在性 ----
require_files \
  'server/gateway/include/mmo/gateway/route/node_registry.h' \
  'server/gateway/include/mmo/gateway/route/route_cache.h' \
  'server/gateway/include/mmo/gateway/route/player_router.h' \
  'server/gateway/include/mmo/gateway/route/scene_router.h' \
  'server/gateway/include/mmo/gateway/route/gateway_router.h' \
  'server/gateway/src/route/node_registry.cpp' \
  'server/gateway/src/route/route_cache.cpp' \
  'server/gateway/src/route/player_router.cpp' \
  'server/gateway/src/route/scene_router.cpp' \
  'server/gateway/src/route/gateway_router.cpp' \
  'server/gateway/tests/route_test.cpp' \
  'server/gateway/tests/route_bench.cpp'

# ---- 3. 模块边界：公开头不得 include 内部 src/ ----
if [ -d "$ROOT/server/gateway/include" ]; then
  scan_forbidden 'server/gateway/include' '#include\s+["<][^">]*src/[^">]*'
fi

# ---- 4. 编译（本地 MinGW + vcpkg，CI 不作为验收依据；Debug + Release 双构建）----
cmake_build_both

# ---- 5. 单元测试（ctest 过滤执行）----
run_ctest "$BUILD_TYPE" 'Gateway_Route' 'Gateway_Route'

# ---- 6. Benchmark 与性能阈值断言 ----
mkdir -p "$ROOT/bench"
run_bench "$BUILD_TYPE" 'bin/route_bench' --lookups 1000000
assert_metric 'bench/gateway_route.txt' 'route_lookup_ns' 'le' '100'
assert_metric 'bench/gateway_route.txt' 'cache_hit_rate' 'ge' '0.99'

# ---- 7. 验收结论 ----
info "人工复核项（脚本无法自动判定，必须人工确认后勾选）："
info "  [ ] 1. PlayerRouter / SceneRouter 均实现，Scene Owner 唯一性有测试保障（重复 Bind 返回 VERSION_CONFLICT）"
info "  [ ] 2. RouteCache 有界（LRU），默认 100k 条目，命中率 > 99%（benchmark 实测）"
info "  [ ] 3. 节点 Dead 后缓存批量失效（NodeDead 事件 → OnNodeDead → EraseByValue），请求不再打向死节点"
info "  [ ] 4. 全部节点不可用时返回 BUSY 而非崩溃（集成测试 + 单测断言）"
info "  [ ] 5. 路由查询 < 100ns（benchmark 实测 route_lookup_ns）"
info "  [ ] 6. 一致性哈希分布均匀（10 节点偏差 < 15%，单测实测 ~5.8%）"
info "  [ ] 7. 1000 节点注册中心 Tick 扫描 < 100us（benchmark 实测 ~1.7us）"
info "  [ ] 8. Debug / Release 双构建通过，ctest -R Gateway_Route 全绿（13 测试函数 / 60+ 断言）"
info "  [ ] 9. 禁止全局锁：RouteCache 16 分片每片独立 mutex；NodeRegistry 单写者无锁"
info "  [ ] 10. 全链路 Login → Gateway → GameNode → Scene 集成测试断言 Session.GameNodeID / SceneID 非空"

end_task "TASK-010"
