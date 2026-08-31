#!/usr/bin/env bash
# TASK-015 · Movement System —— 本地验收脚本
# 自动生成：python tools/gen/build_tasks.py   （禁止手工编辑，改数据源后重新生成）
# 环境：Windows Git Bash / MSYS2 MinGW；g++ (MinGW MSYS2)；vcpkg manifest mode baseline aae277ac
# 用法：bash scripts/verify/task-015.sh
#      BUILD_TYPE=Debug  bash scripts/verify/task-015.sh
# 红线：不做网络操作、不推送 Git、不写数据库；任一步失败即非零退出。
#
# 注意：本仓库 build 缓存为 Ninja，必须注入 GENERATOR=Ninja（默认 MinGW Makefiles 会与
#       既有 Ninja 缓存冲突），并注入 MMO_PROJECT_ROOT / MMO_TASKS_DIR / VCPKG_ROOT / MinGW PATH。

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
source "$ROOT/scripts/verify/_common.sh"

begin_task "TASK-015" 'Movement System'

# ---- 1. 前置任务门禁：必须全部 STATUS: DONE ----
require_tasks_done 011 014

# ---- 2. 交付物存在性 ----
require_files \
  'server/gamenode/movement/include/mmo/game/movement/movement_system.h' \
  'server/gamenode/movement/include/mmo/game/movement/validator.h' \
  'server/gamenode/movement/src/validator.cpp' \
  'server/gamenode/movement/src/movement_system.cpp' \
  'server/gamenode/movement/tests/movement_test.cpp' \
  'server/gamenode/movement/benchmark/movement_bench.cpp' \
  'server/gamenode/movement/docs/INTERFACE.md' \
  'server/gamenode/movement/docs/PERFORMANCE.md'

# ---- 2b. 接口/算法结构体验证 ----
require_content 'server/gamenode/movement/include/mmo/game/movement/movement_system.h' 'class MovementSystem' '存在 MovementSystem'
require_content 'server/gamenode/movement/include/mmo/game/movement/movement_system.h' 'ApplyCommand' '存在 ApplyCommand'
require_content 'server/gamenode/movement/include/mmo/game/movement/movement_system.h' 'Integrate' '存在 Integrate'
require_content 'server/gamenode/movement/src/validator.cpp' 'ValidateMovement' '存在纯函数校验器'
require_content 'server/gamenode/movement/src/movement_system.cpp' 'aoi_->Move' 'AOI 联动（位置变化后调用 IAoi::Move）'

# ---- 3. 模块边界：公开头不得 include 内部 src/；禁止 DB/Redis/网络（§10/§11/§21） ----
if [ -d "$ROOT/server/gamenode/movement/include" ]; then
  scan_forbidden 'server/gamenode/movement/include' '#include\s+["<][^">]*src/[^">]*'
fi
scan_forbidden 'server/gamenode/movement/src' '/(mysql|redis|grpc|sql::|std::ifstream|#include\s+["<]mmo\/core\/net)/'

# ---- 4. 编译（本地 MinGW + vcpkg，Debug + Release 双构建）----
cmake_build_both

# ---- 5. 单元测试（ctest 过滤执行）----
run_ctest "$BUILD_TYPE" 'Movement' 'Movement'

# ---- 6. Benchmark 与性能阈值断言 ----
mkdir -p "$ROOT/bench"
run_bench "$BUILD_TYPE" 'bin/movement_bench' --entities 1000 --ticks 10000
assert_metric 'bench/movement.txt' 'move_ns_per_entity' 'le' '500'

# ---- 7. 验收结论 ----
info "人工复核项（脚本无法自动判定，必须人工确认后勾选）："
info "  [ ] 1. 五条校验规则全部实现，各有单测与反作弊集成测试"
info "  [ ] 2. Movement 代码中不存在数据库/Redis/网络调用（红线扫描已静态确认）"
info "  [ ] 3. 匀速移动 10 秒位移误差 < 1cm（TestIntegrationPrecision）"
info "  [ ] 4. 1000 实体移动处理 < 600us/Tick（move_ns_per_entity 实测 ≤500，含命令驱动；AOI 联动单独计入 aoi_update_ns）"
info "  [ ] 5. NaN / 极大值 / 越界均被拒绝且不污染状态（TestFailurePaths）"
info "  [ ] 6. 拒绝原因分布指标可采集（Stats().rejected_by_reason，反作弊运营）"
info "  [ ] 7. Debug / Release 双构建通过，ctest -R Movement 全绿"

end_task "TASK-015"
