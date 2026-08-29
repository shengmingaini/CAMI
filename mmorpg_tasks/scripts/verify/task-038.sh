#!/usr/bin/env bash
# TASK-038 · Bot + Load + Chaos + Delivery —— 本地验收脚本
# 自动生成：python tools/gen/build_tasks.py   （禁止手工编辑，改数据源后重新生成）
# 环境：Windows Git Bash / MSYS2 MinGW；g++ (MinGW MSYS2)；vcpkg manifest mode baseline aae277ac
# 用法：bash scripts/verify/task-038.sh
#      BUILD_TYPE=Debug  bash scripts/verify/task-038.sh
# 红线：不做网络操作、不推送 Git、不写数据库；任一步失败即非零退出。

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
source "$ROOT/scripts/verify/_common.sh"

begin_task "TASK-038" 'Bot + Load + Chaos + Delivery'

# ---- 1. 前置任务门禁：必须全部 STATUS: DONE ----
require_tasks_done 025 027 028 034 036 037 039 040 041

# ---- 2. 交付物存在性 ----
require_files \
  'docs/architecture/architecture.md' \
  'docs/architecture/capacity-report.md' \
  'tools/bot/bot.h'

# ---- 3. 模块边界：公开头不得 include 内部 src/ ----
if [ -d "$ROOT/tools/qa + docs/architecture/include" ]; then
  scan_forbidden 'tools/qa + docs/architecture/include' '#include\s+["<][^">]*src/[^">]*'
fi

# ---- 4. 编译（本地 MinGW + vcpkg，CI 不作为验收依据） ----
cmake_build_both

# ---- 5. 单元测试（ctest 过滤执行） ----
run_ctest "$BUILD_TYPE" 'Bot' 'Bot'

# ---- 6. Benchmark 与性能阈值断言 ----
mkdir -p "$ROOT/bench"
run_bench "$BUILD_TYPE" 'bin/bot_bench' --bots 1000 --duration 300
assert_metric 'bench/load_1000.txt' 'tick_p99_ms' 'le' '8'
assert_metric 'bench/load_1000.txt' 'error_rate' 'le' '0.001'

# ---- 7. 验收结论 ----
info "人工复核项（脚本无法自动判定，必须人工确认后勾选）："
info "  [ ] 1. **CCU 阶梯 7 级全部跑完**（100/500/1000/5000/10000/20000/50000），数据完整"
info "  [ ] 2. **七项 Chaos 全部执行**（GameNode Crash / Gateway Crash / Redis Failure / MySQL Failure / Network Partition / CPU Saturation / Memory Pressure），每项有恢复断言"
info "  [ ] 3. 网络模拟六种场景全部覆盖"
info "  [ ] 4. docs/architecture/ 六份文档齐全"
info "  [ ] 5. 交付包在新机器上可复现 100 CCU 冒烟"
info "  [ ] 6. capacity-report.md 如实记录容量上限（不达标不得宣称）"
info "  [ ] 7. 全部历史 benchmark 数据汇总归档"
info "  [ ] 8. Debug / Release 双构建通过，全量 ctest 全绿"

end_task "TASK-038"
