#!/usr/bin/env bash
# TASK-030 · Economic Ledger / Idempotency —— 本地验收脚本
# 自动生成：python tools/gen/build_tasks.py   （禁止手工编辑，改数据源后重新生成）
# 环境：Windows Git Bash / MSYS2 MinGW；g++ (MinGW MSYS2)；vcpkg manifest mode baseline aae277ac
# 用法：bash scripts/verify/task-030.sh
#      BUILD_TYPE=Debug  bash scripts/verify/task-030.sh
# 红线：不做网络操作、不推送 Git、不写数据库；任一步失败即非零退出。

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
source "$ROOT/scripts/verify/_common.sh"

begin_task "TASK-030" 'Economic Ledger / Idempotency'

# ---- 1. 前置任务门禁：必须全部 STATUS: DONE ----
require_tasks_done 001 005 026 028 029

# ---- 2. 交付物存在性 ----
require_files \
  'server/gamenode/economy/include/mmo/game/economy/ledger/ledger.h' \
  'database/migrations/003_ledger.sql' \
  'server/gamenode/economy/include/mmo/game/economy/ledger/idempotency_store.h' \
  'tools/audit/economy_audit.py' \
  'server/gamenode/economy/docs/INTERFACE.md' \
  'docs/economy-failure-test-report.md'

# ---- 3. 模块边界：公开头不得 include 内部 src/ ----
if [ -d "$ROOT/server/gamenode/economy/include" ]; then
  scan_forbidden 'server/gamenode/economy/include' '#include\s+["<][^">]*src/[^">]*'
fi

# ---- 4. 编译（本地 MinGW + vcpkg，CI 不作为验收依据） ----
cmake_build_both

# ---- 5. 单元测试（ctest 过滤执行） ----
run_ctest "$BUILD_TYPE" 'Economy_Ledger' 'Economy_Ledger'

# ---- 6. Benchmark 与性能阈值断言 ----
mkdir -p "$ROOT/bench"
run_bench "$BUILD_TYPE" 'bin/ledger_bench' --ops 10000
assert_metric 'bench/ledger.txt' 'idem_check_ns' 'le' '200'
assert_metric 'bench/ledger.txt' 'mem_bytes_per_entry' 'le' '256'

# ---- 7. 验收结论 ----
info "人工复核项（脚本无法自动判定，必须人工确认后勾选）："
info "  [ ] 1. **五个故障场景全部通过**，每个场景断言「余额/物品变化次数 == 1」"
info "  [ ] 2. 幂等表有 UNIQUE 索引兜底（数据库层也挡得住）"
info "  [ ] 3. 账本哈希链可校验，篡改检测生效（单测：改一条记录后 VerifyChain 返回 false）"
info "  [ ] 4. 1 万次操作后资金守恒，对账工具零差异"
info "  [ ] 5. InFlight 有 TTL，崩进程后能恢复，无悬挂"
info "  [ ] 6. 重复请求返回首次结果而非报错（对客户端友好）"
info "  [ ] 7. Debug / Release 双构建通过，ctest -R Economy_Ledger 全绿"

end_task "TASK-030"
