#!/usr/bin/env bash
# TASK-000 · 项目初始化与仓库规范 —— 本地验收脚本
# 自动生成：python tools/gen/build_tasks.py   （禁止手工编辑，改数据源后重新生成）
# 环境：Windows Git Bash / MSYS2 MinGW；g++ (MinGW MSYS2)；vcpkg manifest mode baseline aae277ac
# 用法：bash scripts/verify/task-000.sh
#      BUILD_TYPE=Debug  bash scripts/verify/task-000.sh
# 红线：不做网络操作、不推送 Git、不写数据库；任一步失败即非零退出。

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
source "$ROOT/scripts/verify/_common.sh"

begin_task "TASK-000" '项目初始化与仓库规范'

# ---- 1. 前置任务门禁：无前置 ----
info "本任务无前置依赖，跳过门禁"

# ---- 2. 交付物存在性 ----
require_files \
  'README.md' \
  'PROJECT_REQUIREMENTS.md' \
  'ARCHITECTURE.md' \
  'DEVELOPMENT.md' \
  'CMakeLists.txt' \
  'vcpkg.json'

# ---- 3. 静态红线扫描 ----
scan_forbidden 'engine' '\bstd::cout\s*<<'
scan_forbidden 'server' '\bstd::cout\s*<<'
scan_forbidden 'engine' '\bprintf\s*\('

# ---- 4. 模块边界：公开头不得 include 内部 src/ ----
if [ -d "$ROOT/build / repo/include" ]; then
  scan_forbidden 'build / repo/include' '#include\s+["<][^">]*src/[^">]*'
fi

# ---- 5. 编译（本地 MinGW + vcpkg，CI 不作为验收依据） ----
cmake_build_both

# ---- 6. 单元测试（ctest 过滤执行） ----
run_ctest "$BUILD_TYPE" 'Core' 'Core'

# ---- 7. 验收结论 ----
info "人工复核项（脚本无法自动判定，必须人工确认后勾选）："
info "  [ ] 1. `cmake -S . -B build/Debug -DCMAKE_BUILD_TYPE=Debug` 与 Release 均 configure 成功"
info "  [ ] 2. Debug / Release 两套均 `cmake --build` 成功，退出码 0"
info "  [ ] 3. `ctest --test-dir build/Release` 可执行且不报错"
info "  [ ] 4. README.md / LICENSE / PROJECT_REQUIREMENTS.md / ARCHITECTURE.md / DEVELOPMENT.md 五份文件存在且非空"
info "  [ ] 5. PROJECT_REQUIREMENTS.md 首屏含「冻结」声明与 RFC 变更流程"
info "  [ ] 6. 根目录下**不存在**任何游戏功能代码（src 中无 combat/scene/aoi 等字样），用 grep 验证"
info "  [ ] 7. .gitignore 生效：`git status --short` 不出现 build/、vcpkg_installed/"
info "  [ ] 8. CI workflow 在 GitHub Actions 上至少触发一次并输出构建日志（失败不影响本地验收结论，但必须记录结论到 docs/ci-status.md）"

end_task "TASK-000"
