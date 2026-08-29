#!/usr/bin/env bash
# ---------------------------------------------------------------------------
# MMORPG 任务验收公共库
# 用途：所有 task-XXX.sh 验收脚本共用本文件，保证「本地编译 + 本地验证」口径一致。
# 运行环境：Windows Git Bash / MSYS2 MinGW；编译器 g++ (MinGW MSYS2) 16.1.0+
# 依赖管理：vcpkg manifest mode，baseline aae277ac
# ---------------------------------------------------------------------------
# 设计红线：
#   1. 本库不做任何网络操作，不推送 Git，不写数据库，不改全局状态。
#   2. 任何一步失败即非零退出（set -e），不允许「警告通过」。
#   3. 只报告真实执行结果，禁止兜底伪造 PASS。
# ---------------------------------------------------------------------------

set -euo pipefail

# ---- 双根：任务包根 vs 项目根 -------------------------------------------
# PACK_ROOT   = 任务包目录（tasks/ 与 scripts/verify/ 所在），由脚本自身位置推导
# PROJECT_ROOT= 仓库根（CMakeLists.txt / engine / server / protocol / cmake / build 所在）
#   默认取 PACK_ROOT 的上一级；若任务包不在仓库根之下，用 MMO_PROJECT_ROOT 显式覆盖
PACK_ROOT="$(cd "$(dirname "${BASH_SOURCE[1]:-${BASH_SOURCE[0]}}")/../.." && pwd)"
PROJECT_ROOT="${MMO_PROJECT_ROOT:-$(cd "$PACK_ROOT/.." && pwd)}"
ROOT="$PROJECT_ROOT"
# 本仓库任务文件位于 mmorpg_tasks/tasks/（而非仓库根 tasks/），用 MMO_TASKS_DIR 显式覆盖
# 任务包位置，避免依赖脚本自身路径推导。生成器源 mmorpg_tasks/scripts/verify/_common.sh 未改。
TASK_DIR="${MMO_TASKS_DIR:-$PACK_ROOT/tasks}"
BUILD_ROOT="${BUILD_ROOT:-$PROJECT_ROOT/build}"
BUILD_TYPE="${BUILD_TYPE:-Release}"
VCPKG_ROOT="${VCPKG_ROOT:-${VCPKG_INSTALLATION_ROOT:-$HOME/vcpkg}}"
GENERATOR="${GENERATOR:-MinGW Makefiles}"
MAKE_CMD="${MAKE_CMD:-mingw32-make}"
CTEST_BIN="${CTEST_BIN:-ctest}"
PARALLEL="${PARALLEL:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)}"

# ---- 工具链探测：Git Bash 默认 PATH 里没有 MinGW，这里兜底注入 ------------
MSYS_CANDIDATES=("/c/msys64/mingw64/bin" "/c/msys64/ucrt64/bin" "/c/msys32/mingw32/bin")
for _c in "${MSYS_CANDIDATES[@]}"; do
  if [ -d "$_c" ] && [ -x "$_c/g++.exe" ]; then
    case ":$PATH:" in
      *":$_c:"*) ;;
      *) PATH="$_c:$PATH"; export PATH ;;
    esac
    break
  fi
done
unset MSYS_CANDIDATES

C_RED='\033[0;31m'; C_GRN='\033[0;32m'; C_YEL='\033[0;33m'; C_DIM='\033[2m'; C_OFF='\033[0m'

# 工具链缺失直接失败：没有编译器，后面所有步骤都无意义，不允许「警告通过」
require_toolchain() {
  command -v g++   >/dev/null 2>&1 || die "找不到 g++。请在 MSYS2 MinGW64 shell 中运行，或把 /c/msys64/mingw64/bin 加入 PATH"
  command -v cmake >/dev/null 2>&1 || die "找不到 cmake。请在 MSYS2 MinGW64 shell 中运行，或把 /c/msys64/mingw64/bin 加入 PATH"
  command -v ctest >/dev/null 2>&1 || die "找不到 ctest。请在 MSYS2 MinGW64 shell 中运行，或把 /c/msys64/mingw64/bin 加入 PATH"
  ok "工具链就绪：$(g++ --version | head -1) | cmake $(cmake --version | head -1 | awk '{print $3}')"
}

_log()  { printf "%b\n" "$*"; }
step()  { printf "%b==> %s%b\n" "$C_YEL" "$*" "$C_OFF"; }
ok()    { printf "%b  [OK] %s%b\n" "$C_GRN" "$*" "$C_OFF"; }
bad()   { printf "%b  [FAIL] %s%b\n" "$C_RED" "$*" "$C_OFF" >&2; }
info()  { printf "%b  %s%b\n" "$C_DIM" "$*" "$C_OFF"; }

begin_task() {
  local id="$1"
  printf "\n%b===== %s 验收开始 | build=%s | jobs=%s =====%b\n" \
    "$C_YEL" "$id" "$BUILD_TYPE" "$PARALLEL" "$C_OFF"
  info "repo: $ROOT"
  require_toolchain
  if [ -n "${VCPKG_ROOT:-}" ] && [ -f "$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" ]; then
    ok "vcpkg toolchain: $VCPKG_ROOT"
  else
    info "警告：未检测到 vcpkg.cmake（VCPKG_ROOT=$VCPKG_ROOT），configure 会失败"
  fi
}

end_task() {
  local id="$1"
  printf "%b===== %s 验收通过 =====%b\n" "$C_GRN" "$id" "$C_OFF"
  info "下一步：bash scripts/task-done.sh ${id} && git commit"
}

die() { bad "$*"; exit 1; }

# ---- 任务依赖门禁：前置任务必须处于 STATUS: DONE -------------------------
require_tasks_done() {
  local d
  for d in "$@"; do
    local f="$TASK_DIR/TASK-$d.md"
    [ -f "$f" ] || die "前置任务文件缺失：$TASK_DIR/TASK-$d.md"
    grep -qE '^STATUS:[[:space:]]*DONE[[:space:]]*$' "$f" \
      || die "前置任务 TASK-$d 尚未 DONE（当前 $(grep -m1 '^STATUS:' "$f" || echo '未知')），禁止越级实施"
    ok "前置依赖 TASK-$d = DONE"
  done
}

# ---- 交付物存在性检查 ----------------------------------------------------
require_files() {
  local f
  for f in "$@"; do
    [ -e "$ROOT/$f" ] || die "交付物缺失：$f"
    ok "交付物存在：$f"
  done
}

# ---- 交付物内容检查：文件内必须出现指定正则 ------------------------------
require_content() {
  local f="$1" pat="$2" desc="${3:-$2}"
  [ -f "$ROOT/$f" ] || die "内容检查失败，文件不存在：$f"
  grep -qE "$pat" "$ROOT/$f" || die "内容检查失败：$f 未匹配 /$pat/（$desc）"
  ok "内容检查通过：$f ← $desc"
}

# ---- CMake 配置 ----------------------------------------------------------
cmake_configure() {
  local bt="${1:-$BUILD_TYPE}"
  step "cmake configure ($bt)"
  local dir="$BUILD_ROOT/$bt"
  mkdir -p "$dir"
  cmake -S "$ROOT" -B "$dir" -G "$GENERATOR" \
    -DCMAKE_BUILD_TYPE="$bt" \
    -DCMAKE_TOOLCHAIN_FILE="$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" \
    -DMMORPG_BUILD_TESTS=ON \
    -DMMORPG_BUILD_BENCHMARKS=ON \
    || die "cmake configure 失败（$bt）。禁止跳过，先修根因。"
  ok "cmake configure 通过（$bt）"
}

# ---- 编译 ----------------------------------------------------------------
cmake_build() {
  local bt="${1:-$BUILD_TYPE}" target="${2:-all}"
  step "cmake build ($bt / $target)"
  cmake --build "$BUILD_ROOT/$bt" --target "$target" -j "$PARALLEL" \
    || die "编译失败（$bt）。禁止注释代码绕过，禁止 -k 忽略错误。"
  ok "编译通过（$bt / $target）"
}

# ---- Debug + Release 双构建（基础类任务要求） ----------------------------
cmake_build_both() {
  cmake_configure Debug;   cmake_build Debug
  cmake_configure Release; cmake_build Release
}

# ---- ctest ---------------------------------------------------------------
run_ctest() {
  local bt="${1:-$BUILD_TYPE}" pattern="$2" label="${3:-$2}"
  step "ctest -R '$pattern' ($label)"
  ( cd "$BUILD_ROOT/$bt" && "$CTEST_BIN" --output-on-failure -R "$pattern" ) \
    || die "测试失败：$label（ctest -R '$pattern'）"
  ok "测试通过：$label"
}

# ---- Benchmark -----------------------------------------------------------
run_bench() {
  local bt="${1:-$BUILD_TYPE}" rel="$2"; shift 2 || true
  local exe="$BUILD_ROOT/$bt/$rel"
  [ -x "$exe" ] || exe="$exe.exe"
  [ -x "$exe" ] || die "benchmark 可执行缺失：$rel（先确认 CMake 已开启 MMORPG_BUILD_BENCHMARKS）"
  step "benchmark: $rel $*"
  "$exe" "$@" || die "benchmark 执行失败：$rel"
  ok "benchmark 完成：$rel"
}

# ---- 性能阈值断言：从 benchmark 输出的 key=value 中提取 ------------------
# 用法：assert_metric "bench 输出文件" "metric_key" "比较符(le/lt/ge)" "阈值"
assert_metric() {
  local file="$1" key="$2" op="$3" thr="$4"
  local val
  val="$(grep -oE "^${key}=[0-9.]+" "$file" | tail -1 | cut -d= -f2)"
  [ -n "$val" ] || die "指标缺失：${key}（文件 $file 未输出该指标，禁止用估算值代替）"
  case "$op" in
    le) awk -v v="$val" -v t="$thr" 'BEGIN{exit !(v<=t)}' || die "指标超标：${key}=${val} > ${thr}" ;;
    lt) awk -v v="$val" -v t="$thr" 'BEGIN{exit !(v<t)}'  || die "指标超标：${key}=${val} >= ${thr}" ;;
    ge) awk -v v="$val" -v t="$thr" 'BEGIN{exit !(v>=t)}' || die "指标不足：${key}=${val} < ${thr}" ;;
    *)  die "不支持的比较符：$op" ;;
  esac
  ok "指标达标：${key}=${val} (${op} ${thr})"
}

# ---- 静态红线扫描：禁止裸日志 / 禁止热路径同步 IO ------------------------
scan_forbidden() {
  local dir="$1"; shift
  local pat
  for pat in "$@"; do
    if grep -rnE "$pat" "$ROOT/$dir" --include='*.cpp' --include='*.h' --include='*.hpp' >/dev/null 2>&1; then
      die "红线扫描命中：$dir 内出现 /$pat/（命中文件见上，禁止提交）"
    fi
    ok "红线扫描通过：$dir 无 /$pat/"
  done
}

# ---- 端口占用检查 --------------------------------------------------------
require_free_port() {
  local p="$1"
  if netstat -ano 2>/dev/null | grep -qE "[.:]${p}[[:space:]]"; then
    die "端口 $p 已被占用，先释放或使用 MMORPG_PORT_OFFSET 偏移"
  fi
  ok "端口可用：$p"
}
