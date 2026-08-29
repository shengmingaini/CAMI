#!/usr/bin/env bash
# TASK-032 · Lua Hot Reload —— 本地验收脚本
# 自动生成：python tools/gen/build_tasks.py   （禁止手工编辑，改数据源后重新生成）
# 环境：Windows Git Bash / MSYS2 MinGW；g++ (MinGW MSYS2)；vcpkg manifest mode baseline aae277ac
# 用法：bash scripts/verify/task-032.sh
#      BUILD_TYPE=Debug  bash scripts/verify/task-032.sh
# 红线：不做网络操作、不推送 Git、不写数据库；任一步失败即非零退出。

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
source "$ROOT/scripts/verify/_common.sh"

begin_task "TASK-032" 'Lua Hot Reload'

# ---- 1. 前置任务门禁：必须全部 STATUS: DONE ----
require_tasks_done 003 013 031

# ---- 2. 交付物存在性 ----
require_files \
  'scripting/lua/include/mmo/script/hot_reload/hot_reloader.h' \
  'docs/script-versions.md'

# ---- 3. 模块边界：公开头不得 include 内部 src/ ----
if [ -d "$ROOT/scripting/lua/include" ]; then
  scan_forbidden 'scripting/lua/include' '#include\s+["<][^">]*src/[^">]*'
fi

# ---- 4. 编译（本地 MinGW + vcpkg，CI 不作为验收依据） ----
cmake_build_both

# ---- 5. 单元测试（ctest 过滤执行） ----
run_ctest "$BUILD_TYPE" 'HotReload' 'HotReload'

# ---- 6. Benchmark 与性能阈值断言 ----
mkdir -p "$ROOT/bench"
run_bench "$BUILD_TYPE" 'bin/hotreload_bench' --scripts 100 --reloads 10
assert_metric 'bench/hotreload.txt' 'activate_us' 'le' '100'

# ---- 7. 验收结论 ----
info "人工复核项（脚本无法自动判定，必须人工确认后勾选）："
info "  [ ] 1. **Activate 只能在 Tick Safe Point 执行**（非安全点调用返回错误，单测断言）"
info "  [ ] 2. 编译失败 / 校验失败时**线上旧版本完全不受影响**（集成测试断言）"
info "  [ ] 3. 单次 Activate 停顿 < 100us（不产生 Tick 尖峰，benchmark 实测）"
info "  [ ] 4. 自动回滚生效（注入会崩的脚本，验证自动恢复旧版）"
info "  [ ] 5. 版本历史保留最近 5 个版本，可回滚"
info "  [ ] 6. 每次激活有审计日志与 docs/script-versions.md 记录"
info "  [ ] 7. Debug / Release 双构建通过，ctest -R HotReload 全绿"

end_task "TASK-032"
