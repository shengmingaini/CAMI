#!/usr/bin/env bash
# TASK-033 · Gameplay Script —— 本地验收脚本
# 自动生成：python tools/gen/build_tasks.py   （禁止手工编辑，改数据源后重新生成）
# 环境：Windows Git Bash / MSYS2 MinGW；g++ (MinGW MSYS2)；vcpkg manifest mode baseline aae277ac
# 用法：bash scripts/verify/task-033.sh
#      BUILD_TYPE=Debug  bash scripts/verify/task-033.sh
# 红线：不做网络操作、不推送 Git、不写数据库；任一步失败即非零退出。

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
source "$ROOT/scripts/verify/_common.sh"

begin_task "TASK-033" 'Gameplay Script'

# ---- 1. 前置任务门禁：必须全部 STATUS: DONE ----
require_tasks_done 018 019 021 031 032

# ---- 2. 交付物存在性 ----
require_files \
  'config/gameplay/scripts.json' \
  'scripting/gameplay/skill/fireball.lua' \
  'scripting/gameplay/docs/README.md' \
  'docs/gameplay-script-report.md'

# ---- 3. 模块边界：公开头不得 include 内部 src/ ----
if [ -d "$ROOT/scripting/gameplay/include" ]; then
  scan_forbidden 'scripting/gameplay/include' '#include\s+["<][^">]*src/[^">]*'
fi

# ---- 4. 编译（本地 MinGW + vcpkg，CI 不作为验收依据） ----
cmake_build_both

# ---- 5. 单元测试（ctest 过滤执行） ----
run_ctest "$BUILD_TYPE" 'GameplayScript' 'GameplayScript'

# ---- 6. Benchmark 与性能阈值断言 ----
mkdir -p "$ROOT/bench"
run_bench "$BUILD_TYPE" 'bin/gameplay_script_bench' --iterations 100000
assert_metric 'bench/gameplay.txt' 'skill_formula_ns' 'le' '3000'
assert_metric 'bench/gameplay.txt' 'script_total_cpu_percent' 'le' '10'

# ---- 7. 验收结论 ----
info "人工复核项（脚本无法自动判定，必须人工确认后勾选）："
info "  [ ] 1. 五类脚本（Quest / Skill Formula / NPC AI / Boss Phase / Event）各至少 3 个，共 ≥ 15 个"
info "  [ ] 2. **端到端玩法链路跑通**（集成测试，见上）"
info "  [ ] 3. 脚本路径配置化，C++ 无硬编码脚本名"
info "  [ ] 4. 脚本违反红线（改 HP / 做 IO）被拦截（单测）"
info "  [ ] 5. 热更一个技能公式，线上即时生效且可回滚（实战演练）"
info "  [ ] 6. 脚本总 CPU 占比 < 10%（benchmark 实测，禁止把所有计算塞进 Lua）"
info "  [ ] 7. Debug / Release 双构建通过，ctest -R GameplayScript 全绿"

end_task "TASK-033"
