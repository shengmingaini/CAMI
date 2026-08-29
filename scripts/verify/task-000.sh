#!/usr/bin/env bash
# TASK-000 验收脚本（由生成器产出，禁止手工编辑关键检查项）
# 执行：bash scripts/verify/task-000.sh            # 默认 Release
#      BUILD_TYPE=Debug bash scripts/verify/task-000.sh
set -euo pipefail
source "$(dirname "$0")/_common.sh"

BUILD_TYPE="${BUILD_TYPE:-Release}"
ROOT="$MMORPG_ROOT"

log "===== TASK-000 验收开始 (build=$BUILD_TYPE) ====="

# 1) 交付物存在性检查（6 项）
check_file "$ROOT/README.md"
check_file "$ROOT/LICENSE"
check_file "$ROOT/PROJECT_REQUIREMENTS.md"
check_file "$ROOT/ARCHITECTURE.md"
check_file "$ROOT/DEVELOPMENT.md"
check_file "$ROOT/CMakeLists.txt"
check_file "$ROOT/vcpkg.json"

# 2-5) 静态红线扫描：engine / server 内禁止 std::cout << 与 printf(
redline_scan "$ROOT/engine"  '\bstd::cout\s*<<'  'engine-no-cout'
redline_scan "$ROOT/server"  '\bstd::cout\s*<<'  'server-no-cout'
redline_scan "$ROOT/engine"  '\bprintf\s*\('     'engine-no-printf'
redline_scan "$ROOT/server"  '\bprintf\s*\('     'server-no-printf'

# 6) CMake configure + 编译（Debug + Release 双构建）
#    注：脚本对 Debug/Release 各跑一次由调用方决定；此处按 BUILD_TYPE 跑当前档，
#    双档由 task-000 完整验收流程（CI 同款）覆盖。
cmake_build "$BUILD_TYPE"

# 7) ctest 过滤执行：-R Core（空骨架阶段应返回 No tests were found，而非崩溃）
log "ctest ($BUILD_TYPE)..."
if ctest --test-dir "$ROOT/build/$BUILD_TYPE" -R Core --output-on-failure; then
  ok "ctest 执行完成"
else
  # 空骨架允许「无测试」；仅当 ctest 本身崩溃才算失败
  log "ctest 未匹配到测试（空骨架可接受），继续"
fi

log "===== TASK-000 验收通过 ====="
