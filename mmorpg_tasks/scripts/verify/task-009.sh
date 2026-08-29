#!/usr/bin/env bash
# TASK-009 · Session 管理 —— 本地验收脚本
# 自动生成：python tools/gen/build_tasks.py   （禁止手工编辑，改数据源后重新生成）
# 环境：Windows Git Bash / MSYS2 MinGW；g++ (MinGW MSYS2)；vcpkg manifest mode baseline aae277ac
# 用法：bash scripts/verify/task-009.sh
#      BUILD_TYPE=Debug  bash scripts/verify/task-009.sh
# 红线：不做网络操作、不推送 Git、不写数据库；任一步失败即非零退出。

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
source "$ROOT/scripts/verify/_common.sh"

begin_task "TASK-009" 'Session 管理'

# ---- 1. 前置任务门禁：必须全部 STATUS: DONE ----
require_tasks_done 008

# ---- 2. 交付物存在性 ----
require_files \
  'server/gateway/include/mmo/gateway/session/session_manager.h'

# ---- 3. 模块边界：公开头不得 include 内部 src/ ----
if [ -d "$ROOT/server/gateway/include" ]; then
  scan_forbidden 'server/gateway/include' '#include\s+["<][^">]*src/[^">]*'
fi

# ---- 4. 编译（本地 MinGW + vcpkg，CI 不作为验收依据） ----
cmake_build_both

# ---- 5. 单元测试（ctest 过滤执行） ----
run_ctest "$BUILD_TYPE" 'Gateway_Session' 'Gateway_Session'

# ---- 6. Benchmark 与性能阈值断言 ----
mkdir -p "$ROOT/bench"
run_bench "$BUILD_TYPE" 'bin/session_bench' --sessions 10000
assert_metric 'bench/gateway_session.txt' 'session_tick_us_10k' 'le' '1000'
assert_metric 'bench/gateway_session.txt' 'per_session_bytes' 'le' '256'

# ---- 7. 验收结论 ----
info "人工复核项（脚本无法自动判定，必须人工确认后勾选）："
info "  [ ] 1. Session 七项字段（SessionID/PlayerID/GatewayID/GameNodeID/SceneID/Version + 心跳）全部实现"
info "  [ ] 2. 六状态机全部路径有单测，非法转移返回 INVALID_ARGUMENT"
info "  [ ] 3. version 不匹配的 Reattach 被拒绝（防回放，单测断言）"
info "  [ ] 4. grace 期内可重连、超期释放，两个边界都有测试"
info "  [ ] 5. 1000 会话心跳 Tick 扫描 < 1ms（benchmark 实测）"
info "  [ ] 6. 会话容量上限生效，不无界增长"
info "  [ ] 7. SessionManager 不使用全局锁（代码评审确认）"
info "  [ ] 8. Debug / Release 双构建通过，ctest -R Gateway_Session 全绿"

end_task "TASK-009"
