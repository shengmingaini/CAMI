#!/usr/bin/env bash
# TASK-039 · Social System（组队/好友/公会/聊天/邮件） —— 本地验收脚本
# 自动生成：python tools/gen/build_tasks.py   （禁止手工编辑，改数据源后重新生成）
# 环境：Windows Git Bash / MSYS2 MinGW；g++ (MinGW MSYS2)；vcpkg manifest mode baseline aae277ac
# 用法：bash scripts/verify/task-039.sh
#      BUILD_TYPE=Debug  bash scripts/verify/task-039.sh
# 红线：不做网络操作、不推送 Git、不写数据库；任一步失败即非零退出。

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
source "$ROOT/scripts/verify/_common.sh"

begin_task "TASK-039" 'Social System（组队/好友/公会/聊天/邮件）'

# ---- 1. 前置任务门禁：必须全部 STATUS: DONE ----
require_tasks_done 007 011 016 028

# ---- 2. 交付物存在性 ----
require_files \
  'server/gamenode/social/include/mmo/game/social/social_system.h'

# ---- 3. 模块边界：公开头不得 include 内部 src/ ----
if [ -d "$ROOT/server/gamenode/social/include" ]; then
  scan_forbidden 'server/gamenode/social/include' '#include\s+["<][^">]*src/[^">]*'
fi

# ---- 4. 编译（本地 MinGW + vcpkg，CI 不作为验收依据） ----
cmake_build_both

# ---- 5. 单元测试（ctest 过滤执行） ----
run_ctest "$BUILD_TYPE" 'Social' 'Social'

# ---- 6. 验收结论 ----
info "人工复核项（脚本无法自动判定，必须人工确认后勾选）："
info "  [ ] 1. Party/Friend/Guild/Chat/Mail 五项全部可运行并集成测试通过"
info "  [ ] 2. 所有社交写操作经 Command/Event，grep 确认无直接改 Role/Scene 私有成员"
info "  [ ] 3. Guild 内存态与 MySQL 持久化一致（集成测试断言往返）"
info "  [ ] 4. 世界频道走 Gateway 订阅表，禁止出现全服玩家遍历广播（代码评审 + 静态审查）"
info "  [ ] 5. 邮件领取幂等：重复领取只发一次附件（复用 TASK-030 幂等键）"
info "  [ ] 6. INTERFACE.md 列出导出头与消费的上游接口；`include/` 公开头未泄露 `src/`"
info "  [ ] 7. Debug / Release 双构建通过，ctest -R Social 全绿"

end_task "TASK-039"
