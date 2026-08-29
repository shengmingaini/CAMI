#!/usr/bin/env bash
# TASK-015 · Movement System —— 本地验收脚本
# 自动生成：python tools/gen/build_tasks.py   （禁止手工编辑，改数据源后重新生成）
# 环境：Windows Git Bash / MSYS2 MinGW；g++ (MinGW MSYS2)；vcpkg manifest mode baseline aae277ac
# 用法：bash scripts/verify/task-015.sh
#      BUILD_TYPE=Debug  bash scripts/verify/task-015.sh
# 红线：不做网络操作、不推送 Git、不写数据库；任一步失败即非零退出。

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
source "$ROOT/scripts/verify/_common.sh"

begin_task "TASK-015" 'Movement System'

# ---- 1. 前置任务门禁：必须全部 STATUS: DONE ----
require_tasks_done 011 014

# ---- 2. 交付物存在性 ----
require_files \
  'server/gamenode/movement/include/mmo/game/movement/movement_system.h'

# ---- 3. 静态红线扫描 ----
scan_forbidden 'server/gamenode/movement/src' '(mysql|redis|grpc|sql::|std::ifstream)'

# ---- 4. 模块边界：公开头不得 include 内部 src/ ----
if [ -d "$ROOT/server/gamenode/movement/include" ]; then
  scan_forbidden 'server/gamenode/movement/include' '#include\s+["<][^">]*src/[^">]*'
fi

# ---- 5. 编译（本地 MinGW + vcpkg，CI 不作为验收依据） ----
cmake_build_both

# ---- 6. 单元测试（ctest 过滤执行） ----
run_ctest "$BUILD_TYPE" 'Movement' 'Movement'

# ---- 7. Benchmark 与性能阈值断言 ----
mkdir -p "$ROOT/bench"
run_bench "$BUILD_TYPE" 'bin/movement_bench' --entities 1000 --ticks 10000
assert_metric 'bench/movement.txt' 'move_ns_per_entity' 'le' '500'

# ---- 8. 验收结论 ----
info "人工复核项（脚本无法自动判定，必须人工确认后勾选）："
info "  [ ] 1. 五条校验规则全部实现，各有单测与反作弊集成测试"
info "  [ ] 2. **Movement 代码中不存在任何数据库/Redis/网络调用**（红线扫描）"
info "  [ ] 3. 匀速移动 10 秒位移误差 < 1cm（无累积漂移）"
info "  [ ] 4. 1000 实体移动处理 < 600us/Tick（benchmark 实测，对应阶段预算）"
info "  [ ] 5. NaN / 极大值 / 越界均被拒绝且不污染状态（单测）"
info "  [ ] 6. 拒绝原因分布指标可采集（为反作弊运营提供数据）"
info "  [ ] 7. Debug / Release 双构建通过，ctest -R Movement 全绿"

end_task "TASK-015"
