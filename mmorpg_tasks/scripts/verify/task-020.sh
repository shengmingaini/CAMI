#!/usr/bin/env bash
# TASK-020 · World / Instance —— 本地验收脚本
# 自动生成：python tools/gen/build_tasks.py   （禁止手工编辑，改数据源后重新生成）
# 环境：Windows Git Bash / MSYS2 MinGW；g++ (MinGW MSYS2)；vcpkg manifest mode baseline aae277ac
# 用法：bash scripts/verify/task-020.sh
#      BUILD_TYPE=Debug  bash scripts/verify/task-020.sh
# 红线：不做网络操作、不推送 Git、不写数据库；任一步失败即非零退出。

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
source "$ROOT/scripts/verify/_common.sh"

begin_task "TASK-020" 'World / Instance'

# ---- 1. 前置任务门禁：必须全部 STATUS: DONE ----
require_tasks_done 012 018

# ---- 2. 交付物存在性 ----
require_files \
  'server/gamenode/world/include/mmo/game/world/instance_manager.h' \
  'server/gamenode/world/include/mmo/game/world/world_manager.h' \
  'server/gamenode/world/docs/INTERFACE.md' \
  'server/gamenode/world/docs/README.md'

# ---- 3. 模块边界：公开头不得 include 内部 src/ ----
if [ -d "$ROOT/server/gamenode/world/include" ]; then
  scan_forbidden 'server/gamenode/world/include' '#include\s+["<][^">]*src/[^">]*'
fi

# ---- 4. 编译（本地 MinGW + vcpkg，CI 不作为验收依据） ----
cmake_build_both

# ---- 5. 单元测试（ctest 过滤执行） ----
run_ctest "$BUILD_TYPE" 'World' 'World'

# ---- 6. Benchmark 与性能阈值断言 ----
mkdir -p "$ROOT/bench"
run_bench "$BUILD_TYPE" 'bin/world_bench' --instances 100 --duration 600
assert_metric 'bench/world.txt' 'mem_bytes_per_instance' 'le' '4096'
assert_metric 'bench/world.txt' 'tick_us_per_100_instances' 'le' '100'

# ---- 7. 验收结论 ----
info "人工复核项（脚本无法自动判定，必须人工确认后勾选）："
info "  [ ] 1. 五状态（Pending/Loading/Running/Completed/Destroying）全部实现且有单测"
info "  [ ] 2. OpenWorld / Dungeon / Arena 三类场景均可创建运行"
info "  [ ] 3. 100 个并发实例跑 10 分钟后全部回收，Scene 与实体数归零（无泄漏，集成测试断言）"
info "  [ ] 4. 超时、空实例、全员退出三条回收路径均有测试"
info "  [ ] 5. 分线策略生效（300 人触发，分线间不可见）"
info "  [ ] 6. 实例配置全部配置化（代码无硬编码）"
info "  [ ] 7. Debug / Release 双构建通过，ctest -R World 全绿"

end_task "TASK-020"
