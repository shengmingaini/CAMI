#!/usr/bin/env bash
# TASK-029 · Economy System —— 本地验收脚本
# 自动生成：python tools/gen/build_tasks.py   （禁止手工编辑，改数据源后重新生成）
# 环境：Windows Git Bash / MSYS2 MinGW；g++ (MinGW MSYS2)；vcpkg manifest mode baseline aae277ac
# 用法：bash scripts/verify/task-029.sh
#      BUILD_TYPE=Debug  bash scripts/verify/task-029.sh
# 红线：不做网络操作、不推送 Git、不写数据库；任一步失败即非零退出。

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
source "$ROOT/scripts/verify/_common.sh"

begin_task "TASK-029" 'Economy System'

# ---- 1. 前置任务门禁：必须全部 STATUS: DONE ----
require_tasks_done 017 026

# ---- 2. 交付物存在性 ----
require_files \
  'server/gamenode/economy/include/mmo/game/economy/economy_system.h'

# ---- 3. 模块边界：公开头不得 include 内部 src/ ----
if [ -d "$ROOT/server/gamenode/economy/include" ]; then
  scan_forbidden 'server/gamenode/economy/include' '#include\s+["<][^">]*src/[^">]*'
fi

# ---- 4. 编译（本地 MinGW + vcpkg，CI 不作为验收依据） ----
cmake_build_both

# ---- 5. 单元测试（ctest 过滤执行） ----
run_ctest "$BUILD_TYPE" 'Economy' 'Economy'

# ---- 6. Benchmark 与性能阈值断言 ----
mkdir -p "$ROOT/bench"
run_bench "$BUILD_TYPE" 'bin/economy_bench' --ops 10000
assert_metric 'bench/economy.txt' 'execute_ns' 'le' '2000'
assert_metric 'bench/economy.txt' 'balance_query_ns' 'le' '50'

# ---- 7. 验收结论 ----
info "人工复核项（脚本无法自动判定，必须人工确认后勾选）："
info "  [ ] 1. **所有经济操作都经过 EconomyCommand**（grep：无直接改余额的代码路径）"
info "  [ ] 2. 八种操作（Add/RemoveCurrency、Add/RemoveItem、Transfer、Purchase、Reward、Refund）全部实现"
info "  [ ] 3. 余额不足/背包满时拒绝且不产生负余额或丢物品（单测）"
info "  [ ] 4. 缺 idempotency_key 的命令被拒绝（单测）"
info "  [ ] 5. 货币守恒（1 万次操作校验）"
info "  [ ] 6. 价格表配置化（代码无硬编码价格）"
info "  [ ] 7. Debug / Release 双构建通过，ctest -R Economy 全绿"

end_task "TASK-029"
