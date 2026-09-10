#!/usr/bin/env bash
# TASK-014 · AOI System（Dynamic Grid 第一版） —— 本地验收脚本
# 自动生成：python tools/gen/build_tasks.py   （禁止手工编辑，改数据源后重新生成）
# 环境：Windows Git Bash / MSYS2 MinGW；g++ (MinGW MSYS2)；vcpkg manifest mode baseline aae277ac
# 用法：bash scripts/verify/task-014.sh
#      BUILD_TYPE=Debug  bash scripts/verify/task-014.sh
# 红线：不做网络操作、不推送 Git、不写数据库；任一步失败即非零退出。

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
source "$ROOT/scripts/verify/_common.sh"

begin_task "TASK-014" 'AOI System（Dynamic Grid 第一版）'

# ---- 1. 前置任务门禁：必须全部 STATUS: DONE ----
require_tasks_done 011 012

# ---- 2. 交付物存在性 ----
require_files \
  'server/gamenode/aoi/include/mmo/game/aoi/aoi.h' \
  'server/gamenode/aoi/docs/PERFORMANCE.md' \
  'server/gamenode/aoi/include/mmo/game/aoi/dynamic_grid_aoi.h' \
  'server/gamenode/aoi/docs/INTERFACE.md'

# ---- 3. 模块边界：公开头不得 include 内部 src/ ----
if [ -d "$ROOT/server/gamenode/aoi/include" ]; then
  scan_forbidden 'server/gamenode/aoi/include' '#include\s+["<][^">]*src/[^">]*'
fi

# ---- 4. 编译（本地 MinGW + vcpkg，CI 不作为验收依据） ----
cmake_build_both

# ---- 5. 单元测试（ctest 过滤执行） ----
run_ctest "$BUILD_TYPE" 'Aoi' 'Aoi'

# ---- 6. Benchmark 与性能阈值断言 ----
mkdir -p "$ROOT/bench"
run_bench "$BUILD_TYPE" 'bin/aoi_bench' --entities 100,500,1000,2000 --ticks 10000
assert_metric 'bench/aoi_1000.txt' 'query_ns_p99' 'le' '15000'
assert_metric 'bench/aoi_1000.txt' 'mem_bytes_per_entity' 'le' '128'

# ---- 7. 验收结论 ----
info "人工复核项（脚本无法自动判定，必须人工确认后勾选）："
info "  [ ] 1. 五个接口（Enter/Leave/Move/QueryVisible/Broadcast）全部实现且有单测"
info "  [ ] 2. **与暴力 O(N²) 参考实现的可见集 100% 一致**（1000 实体 × 100 次随机布局比对）"
info "  [ ] 3. 100 / 500 / 1000 / 2000 四档基准数据全部产出并写入 docs/PERFORMANCE.md"
info "  [ ] 4. 查询局部化：grep 确认不存在遍历全部实体的代码路径"
info "  [ ] 5. 广播复用缓冲区，不为每个目标单独分配（benchmark 分配计数验证）"
info "  [ ] 6. 坐标 NaN / 越界被拒绝，不产生越界访问"
info "  [ ] 7. Debug / Release 双构建通过，ctest -R Aoi 全绿"

end_task "TASK-014"
