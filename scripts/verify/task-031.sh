#!/usr/bin/env bash
# TASK-031 · Lua Runtime —— 本地验收脚本
# 自动生成：python tools/gen/build_tasks.py   （禁止手工编辑，改数据源后重新生成）
# 环境：Windows Git Bash / MSYS2 MinGW；g++ (MinGW MSYS2)；vcpkg manifest mode baseline aae277ac
# 用法：bash scripts/verify/task-031.sh
#      BUILD_TYPE=Debug  bash scripts/verify/task-031.sh
# 红线：不做网络操作、不推送 Git、不写数据库；任一步失败即非零退出。

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
source "$ROOT/scripts/verify/_common.sh"

begin_task "TASK-031" 'Lua Runtime'

# ---- 1. 前置任务门禁：必须全部 STATUS: DONE ----
require_tasks_done 001 004 005 007 011

# ---- 2. 交付物存在性 ----
require_files \
  'scripting/lua/include/mmo/script/script_context.h' \
  'scripting/lua/docs/SANDBOX.md' \
  'scripting/lua/include/mmo/script/lua_vm.h' \
  'scripting/lua/docs/INTERFACE.md'

# ---- 3. 模块边界：公开头不得 include 内部 src/ ----
if [ -d "$ROOT/scripting/lua/include" ]; then
  scan_forbidden 'scripting/lua/include' '#include\s+["<][^">]*src/[^">]*'
fi

# ---- 4. 编译（本地 MinGW + vcpkg，CI 不作为验收依据） ----
cmake_build_both

# ---- 5. 单元测试（ctest 过滤执行） ----
run_ctest "$BUILD_TYPE" 'Lua' 'Lua'

# ---- 6. Benchmark 与性能阈值断言 ----
mkdir -p "$ROOT/bench"
run_bench "$BUILD_TYPE" 'bin/lua_bench' --calls 1000000
assert_metric 'bench/lua.txt' 'lua_call_ns' 'le' '2000'

# ---- 7. 验收结论 ----
info "人工复核项（脚本无法自动判定，必须人工确认后勾选）："
info "  [ ] 1. **每 Scene 一个 VM，不跨线程共享**（grep + 并发测试）"
info "  [ ] 2. 沙箱生效：io / os / loadstring / require 全部不可用（单测断言）"
info "  [ ] 3. 四类限制（内存/指令/时间/栈深）全部触发正确，且**不卡死 Tick**"
info "  [ ] 4. 脚本错误被捕获，宿主进程不崩溃，Scene 继续 Tick（单测）"
info "  [ ] 5. 五项绑定（Entity/Skill/Quest/Event/Query）可用且只走系统接口"
info "  [ ] 6. 单次脚本调用 < 2us（benchmark 实测）"
info "  [ ] 7. Debug / Release 双构建通过，ctest -R Lua 全绿"

end_task "TASK-031"
