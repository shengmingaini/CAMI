#!/usr/bin/env bash
# TASK-018 · NPC / Monster / AI —— 本地验收脚本
# 自动生成：python tools/gen/build_tasks.py   （禁止手工编辑，改数据源后重新生成）
# 环境：Windows Git Bash / MSYS2 MinGW；g++ (MinGW MSYS2)；vcpkg manifest mode baseline aae277ac
# 用法：bash scripts/verify/task-018.sh
#      BUILD_TYPE=Debug  bash scripts/verify/task-018.sh
# 红线：不做网络操作、不推送 Git、不写数据库；任一步失败即非零退出。

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
source "$ROOT/scripts/verify/_common.sh"

begin_task "TASK-018" 'NPC / Monster / AI'

# ---- 1. 前置任务门禁：必须全部 STATUS: DONE ----
require_tasks_done 011 014 015

# ---- 2. 交付物存在性 ----
require_files \
  'server/gamenode/ai/include/mmo/game/ai/ai_system.h' \
  'server/gamenode/ai/include/mmo/game/ai/ai_state.h' \
  'server/gamenode/ai/include/mmo/game/ai/spawn_def.h' \
  'server/gamenode/ai/docs/INTERFACE.md' \
  'server/gamenode/ai/docs/PERFORMANCE.md'

# ---- 3. 模块边界：公开头不得 include 内部 src/ ----
if [ -d "$ROOT/server/gamenode/ai/include" ]; then
  scan_forbidden 'server/gamenode/ai/include' '#include\s+["<][^">]*src/[^">]*'
fi

# ---- 4. 编译（本地 MinGW + vcpkg，CI 不作为验收依据） ----
cmake_build_both

# ---- 5. 单元测试（ctest 过滤执行） ----
run_ctest "$BUILD_TYPE" 'Ai' 'Ai'

# ---- 6. Benchmark 与性能阈值断言 ----
mkdir -p "$ROOT/bench"
run_bench "$BUILD_TYPE" 'bin/ai_bench' --monsters 1000 --ticks 12000
assert_metric 'bench/ai.txt' 'ai_phase_us_at_1k' 'le' '400'
assert_metric 'bench/ai.txt' 'mem_bytes_per_ai' 'le' '128'

# ---- 7. 验收结论 ----
info "人工复核项（脚本无法自动判定，必须人工确认后勾选）："
info "  [ ] 1. 六态（Idle/Patrol/Chase/Attack/Return/Dead）全部实现，转移表驱动"
info "  [ ] 2. **AI 决策节流生效**：1000 怪 1 秒内决策次数 ≈ 5000（5Hz），而非 20000"
info "  [ ] 3. 目标选取走 AOI 局部查询，无全 Scene 扫描（grep + benchmark 佐证）"
info "  [ ] 4. 死亡与重生走 Scheduler，不创建线程/不每 Buff 一个定时器"
info "  [ ] 5. 1000 怪长稳 10 分钟无死锁、无泄漏、状态分布可观测"
info "  [ ] 6. NPC/Monster 属性全部配置化（代码无硬编码数值）"
info "  [ ] 7. Debug / Release 双构建通过，ctest -R Ai 全绿"

end_task "TASK-018"
