#!/usr/bin/env bash
# TASK-040 · ControlService（控制面：节点管理/配置下发/健康/运维） —— 本地验收脚本
# 自动生成：python tools/gen/build_tasks.py   （禁止手工编辑，改数据源后重新生成）
# 环境：Windows Git Bash / MSYS2 MinGW；g++ (MinGW MSYS2)；vcpkg manifest mode baseline aae277ac
# 用法：bash scripts/verify/task-040.sh
#      BUILD_TYPE=Debug  bash scripts/verify/task-040.sh
# 红线：不做网络操作、不推送 Git、不写数据库；任一步失败即非零退出。

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
source "$ROOT/scripts/verify/_common.sh"

begin_task "TASK-040" 'ControlService（控制面：节点管理/配置下发/健康/运维）'

# ---- 1. 前置任务门禁：必须全部 STATUS: DONE ----
require_tasks_done 003 006 010

# ---- 2. 交付物存在性 ----
require_files \
  'server/control/include/mmo/control/control_service.h'

# ---- 3. 模块边界：公开头不得 include 内部 src/ ----
if [ -d "$ROOT/server/control/include" ]; then
  scan_forbidden 'server/control/include' '#include\s+["<][^">]*src/[^">]*'
fi

# ---- 4. 编译（本地 MinGW + vcpkg，CI 不作为验收依据） ----
cmake_build_both

# ---- 5. 单元测试（ctest 过滤执行） ----
run_ctest "$BUILD_TYPE" 'Control' 'Control'

# ---- 6. 验收结论 ----
info "人工复核项（脚本无法自动判定，必须人工确认后勾选）："
info "  [ ] 1. 节点注册/心跳/超时/配置下发/拓扑查询全部实现并集成测试通过"
info "  [ ] 2. 节点上下线正确触发 TASK-010 路由缓存失效协调"
info "  [ ] 3. config_version 推进后节点热加载新配置（复用 TASK-003 原子替换，无锁）"
info "  [ ] 4. Gateway 多实例注册并输出 capacity/load（前置 LB 分流所需数据齐备）"
info "  [ ] 5. ControlService 崩溃时游戏节点降级不雪崩（集成测试断言）"
info "  [ ] 6. INTERFACE.md 列出导出接口与消费的上游接口；`include/` 未泄露 `src/`"
info "  [ ] 7. Debug / Release 双构建通过，ctest -R Control 全绿"

end_task "TASK-040"
