#!/usr/bin/env bash
# TASK-035 · Renderer —— 本地验收脚本
# 自动生成：python tools/gen/build_tasks.py   （禁止手工编辑，改数据源后重新生成）
# 环境：Windows Git Bash / MSYS2 MinGW；g++ (MinGW MSYS2)；vcpkg manifest mode baseline aae277ac
# 用法：bash scripts/verify/task-035.sh
#      BUILD_TYPE=Debug  bash scripts/verify/task-035.sh
# 红线：不做网络操作、不推送 Git、不写数据库；任一步失败即非零退出。

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
source "$ROOT/scripts/verify/_common.sh"

begin_task "TASK-035" 'Renderer'

# ---- 1. 前置任务门禁：必须全部 STATUS: DONE ----
require_tasks_done 034

# ---- 2. 交付物存在性 ----
require_files \
  'client/renderer/include/mmo/client/render/renderer.h' \
  'client/renderer/docs/PERFORMANCE.md'

# ---- 3. 模块边界：公开头不得 include 内部 src/ ----
if [ -d "$ROOT/client/renderer/include" ]; then
  scan_forbidden 'client/renderer/include' '#include\s+["<][^">]*src/[^">]*'
fi

# ---- 4. 编译（本地 MinGW + vcpkg，CI 不作为验收依据） ----
cmake_build_both

# ---- 5. 单元测试（ctest 过滤执行） ----
run_ctest "$BUILD_TYPE" 'Renderer' 'Renderer'

# ---- 6. Benchmark 与性能阈值断言 ----
mkdir -p "$ROOT/bench"
run_bench "$BUILD_TYPE" 'bin/render_bench' --scene test_scene --quality low --duration 600
assert_metric 'bench/render_low.txt' 'draw_calls' 'le' '300'
assert_metric 'bench/render_low.txt' 'texture_mem_mb' 'le' '512'
assert_metric 'bench/render_low.txt' 'triangles' 'le' '300000'

# ---- 7. 验收结论 ----
info "人工复核项（脚本无法自动判定，必须人工确认后勾选）："
info "  [ ] 1. Camera / Mesh / Material / Texture / Animation / UI 六项全部实现"
info "  [ ] 2. **Low 档 Draw Call < 300、三角面 < 300k、纹理显存 < 512MB**（benchmark 实测）"
info "  [ ] 3. 静态合批与视锥剔除生效（有对照数据：开启前后 Draw Call 对比）"
info "  [ ] 4. LOD 三级生效"
info "  [ ] 5. 画质 Low/Medium/High 可热切换"
info "  [ ] 6. 连续渲染 10 分钟无显存泄漏"
info "  [ ] 7. 设备丢失可恢复，不崩溃"
info "  [ ] 8. Debug / Release 双构建通过，ctest -R Renderer 全绿"

end_task "TASK-035"
