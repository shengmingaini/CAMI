#!/usr/bin/env bash
# TASK-001 · Core Error / Result 系统 —— 本地验收脚本
# 自动生成：python tools/gen/build_tasks.py   （禁止手工编辑，改数据源后重新生成）
# 环境：Windows Git Bash / MSYS2 MinGW；g++ (MinGW MSYS2)；vcpkg manifest mode baseline aae277ac
# 用法：bash scripts/verify/task-001.sh
#      BUILD_TYPE=Debug  bash scripts/verify/task-001.sh
# 红线：不做网络操作、不推送 Git、不写数据库；任一步失败即非零退出。

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
source "$ROOT/scripts/verify/_common.sh"

begin_task "TASK-001" 'Core Error / Result 系统'

# ---- 1. 前置任务门禁：必须全部 STATUS: DONE ----
require_tasks_done 000

# ---- 2. 交付物存在性 ----
require_files \
  'engine/core/include/mmo/core/error/result.h' \
  'engine/core/docs/INTERFACE.md' \
  'engine/core/include/mmo/core/error/error_code.h' \
  'engine/core/include/mmo/core/error/error.h' \
  'engine/core/docs/README.md' \
  'engine/core/docs/PERFORMANCE.md'

# ---- 3. 静态红线扫描 ----
scan_forbidden 'engine/core/src/error' '\bthrow\s+'
scan_forbidden 'engine/core/include/mmo/core/error' '\bthrow\s+'

# ---- 4. 模块边界：公开头不得 include 内部 src/ ----
if [ -d "$ROOT/engine/core/include" ]; then
  scan_forbidden 'engine/core/include' '#include\s+["<][^">]*src/[^">]*'
fi

# ---- 5. 编译（本地 MinGW + vcpkg，CI 不作为验收依据） ----
cmake_build_both

# ---- 6. 单元测试（ctest 过滤执行） ----
run_ctest "$BUILD_TYPE" 'Core_Error' 'Core_Error'

# ---- 7. Benchmark 与性能阈值断言 ----
mkdir -p "$ROOT/bench"
run_bench "$BUILD_TYPE" 'bin/error_bench' --iterations 10000000
assert_metric 'bench/core_error.txt' 'result_ns_per_op' 'le' '5'
assert_metric 'bench/core_error.txt' 'alloc_per_fail' 'le' '0'

# ---- 8. 验收结论 ----
info "人工复核项（脚本无法自动判定，必须人工确认后勾选）："
info "  [ ] 1. 9 个错误码全部实现，ToString/FromString 双向一致（单测覆盖）"
info "  [ ] 2. Result<T> 与 Result<void> 均带 `[[nodiscard]]`，丢弃返回值在 -Wall -Wextra 下产生告警"
info "  [ ] 3. 单元测试全部通过（ctest -R Core_Error）"
info "  [ ] 4. benchmark 输出 `result_ns_per_op` 且失败路径 `alloc_per_fail=0`"
info "  [ ] 5. grep 全仓：除 engine/core 外**不存在**第二个 enum class ErrorCode 定义"
info "  [ ] 6. Debug / Release 双构建通过"
info "  [ ] 7. engine/core/docs/README.md 与 INTERFACE.md 存在且含使用示例"

end_task "TASK-001"
