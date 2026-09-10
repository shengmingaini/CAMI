#!/usr/bin/env bash
# TASK-025 · Combat Benchmark（架构可行性判定点） —— 本地验收脚本
# 自动生成：python tools/gen/build_tasks.py   （禁止手工编辑，改数据源后重新生成）
# 环境：Windows Git Bash / MSYS2 MinGW；g++ (MinGW MSYS2)；vcpkg manifest mode baseline aae277ac
# 用法：bash scripts/verify/task-025.sh
#      BUILD_TYPE=Debug  bash scripts/verify/task-025.sh
# 红线：不做网络操作、不推送 Git、不写数据库；任一步失败即非零退出。

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
source "$ROOT/scripts/verify/_common.sh"

begin_task "TASK-025" 'Combat Benchmark（架构可行性判定点）'

# ---- 1. 前置任务门禁：必须全部 STATUS: DONE ----
require_tasks_done 013 014 015 024

# ---- 2. 交付物存在性 ----
require_files \
  'docs/benchmark/combat-report.md' \
  'benchmark/combat/combat_benchmark.h' \
  'tools/report/compare.py' \
  'docs/benchmark/matrix.json'

# ---- 3. 模块边界：公开头不得 include 内部 src/ ----
if [ -d "$ROOT/benchmark/combat/include" ]; then
  scan_forbidden 'benchmark/combat/include' '#include\s+["<][^">]*src/[^">]*'
fi

# ---- 4. 编译（本地 MinGW + vcpkg，CI 不作为验收依据） ----
cmake_configure "$BUILD_TYPE"
cmake_build "$BUILD_TYPE"

# ---- 5. 单元测试（ctest 过滤执行） ----
run_ctest "$BUILD_TYPE" 'Bench_Combat' 'Bench_Combat'

# ---- 6. Benchmark 与性能阈值断言 ----
mkdir -p "$ROOT/bench"
run_bench "$BUILD_TYPE" 'bin/combat_bench' --matrix --duration 60 --warmup 5 --out bench/combat_matrix.json
assert_metric 'bench/combat_1000_100pct.txt' 'tick_p95_us' 'le' '5000'
assert_metric 'bench/combat_1000_100pct.txt' 'tick_p99_us' 'le' '8000'
assert_metric 'bench/combat_1000_100pct.txt' 'tick_avg_us' 'lt' '5000'

# ---- 7. 验收结论 ----
info "人工复核项（脚本无法自动判定，必须人工确认后勾选）："
info "  [ ] 1. **5 场景 × 4 规模 = 20 组全部跑完**（禁止抽样，报告必须含完整矩阵）"
info "  [ ] 2. 每组输出 Average / P50 / P95 / P99 / Max Tick 与八阶段 P95 分解"
info "  [ ] 3. 1000 玩家场景：Average < 5ms、P95 ≤ 5ms、P99 ≤ 8ms（最差场景即 100% Combat 也要达标；若 Idle 达标而 Combat 不达标，判定**不通过**）"
info "  [ ] 4. 报告含 CPU / 内存 / 消息量数据"
info "  [ ] 5. 同配置可复现（两次结果差异 < 5%）"
info "  [ ] 6. docs/benchmark/combat-report.md 存在且含明确「通过/不通过」结论"
info "  [ ] 7. 若不通过，docs/perf-analysis.md 存在且含瓶颈定位与改进方案（并暂停后续任务）"

end_task "TASK-025"
