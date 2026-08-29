#!/usr/bin/env bash
# TASK-036 · Resource / Low Spec System —— 本地验收脚本
# 自动生成：python tools/gen/build_tasks.py   （禁止手工编辑，改数据源后重新生成）
# 环境：Windows Git Bash / MSYS2 MinGW；g++ (MinGW MSYS2)；vcpkg manifest mode baseline aae277ac
# 用法：bash scripts/verify/task-036.sh
#      BUILD_TYPE=Debug  bash scripts/verify/task-036.sh
# 红线：不做网络操作、不推送 Git、不写数据库；任一步失败即非零退出。

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
source "$ROOT/scripts/verify/_common.sh"

begin_task "TASK-036" 'Resource / Low Spec System'

# ---- 1. 前置任务门禁：必须全部 STATUS: DONE ----
require_tasks_done 034 035

# ---- 2. 交付物存在性 ----
require_files \
  'config/client/quality.json' \
  'client/resource/include/mmo/client/resource/scene_streamer.h'

# ---- 3. 模块边界：公开头不得 include 内部 src/ ----
if [ -d "$ROOT/client/resource/include" ]; then
  scan_forbidden 'client/resource/include' '#include\s+["<][^">]*src/[^">]*'
fi

# ---- 4. 编译（本地 MinGW + vcpkg，CI 不作为验收依据） ----
cmake_build_both

# ---- 5. 单元测试（ctest 过滤执行） ----
run_ctest "$BUILD_TYPE" 'Resource' 'Resource'

# ---- 6. Benchmark 与性能阈值断言 ----
mkdir -p "$ROOT/bench"
run_bench "$BUILD_TYPE" 'bin/resource_bench' --quality low --duration 300
assert_metric 'bench/resource_low.txt' 'ram_mb' 'le' '1536'
assert_metric 'bench/resource_low.txt' 'vram_mb' 'le' '1024'
assert_metric 'bench/resource_low.txt' 'draw_calls' 'le' '300'

# ---- 7. 验收结论 ----
info "人工复核项（脚本无法自动判定，必须人工确认后勾选）："
info "  [ ] 1. **Current Chunk + Nearby Chunk 策略生效**，远处自动释放（集成测试：5 分钟跑图 RAM 平稳）"
info "  [ ] 2. Low / Medium / High 三档全部可用且可热切换"
info "  [ ] 3. **Low 档 RAM < 1.5GB、VRAM < 1GB、Draw Call < 300**（实测，写入 PERFORMANCE.md）"
info "  [ ] 4. 三类缓存各自独立预算，互不挤占（单测）"
info "  [ ] 5. 来回穿越 Chunk 边界不触发加载风暴（迟滞生效）"
info "  [ ] 6. 低配实测脚本可输出完整资源曲线报告"
info "  [ ] 7. Debug / Release 双构建通过，ctest -R Resource 全绿"

end_task "TASK-036"
