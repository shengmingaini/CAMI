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
# 任务包位置，避免依赖脚本自身路径推导。此文件已与任务包源 mmorpg_tasks/scripts/verify/_common.sh
# 保持同步（两份内容一致），避免重新播种任务包时丢失本仓库的修复。
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
# 兼容别名：早期生成的验收脚本（如 TASK-000）调用 log()，与 info() 同义。
log()   { info "$@"; }

# 兼容别名（早期脚本 API）：check_file <绝对路径> —— 单文件存在性，缺失即失败。
check_file() {
  local f="$1"
  [ -e "$f" ] || die "交付物缺失：$f"
  ok "交付物存在：$f"
}

# 兼容别名（早期脚本 API）：redline_scan <绝对路径目录> <正则> [说明]
# 与 scan_forbidden 的区别：接受绝对路径、且第三个参数是「说明」而非额外正则。
redline_scan() {
  local dir="$1" pat="$2"
  if grep -rnE "$pat" "$dir" --include='*.cpp' --include='*.h' --include='*.hpp' >/dev/null 2>&1; then
    die "红线扫描命中：$dir 内出现 /$pat/（命中文件见上，禁止提交）"
  fi
  ok "红线扫描通过：$dir 无 /$pat/"
}

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
  if [ "${MMO_OFFLINE:-0}" = "1" ]; then
    # 离线模式：跳过 vcpkg manifest 安装，改走系统 MinGW 库（Windows 风格编译器/构建器路径，
    # 规避 MSYS 路径在全新 shell 下无法 spawn ninja 的问题）。与本地手动验证口径一致。
    cmake -S "$ROOT" -B "$dir" -G "$GENERATOR" \
      -DCMAKE_BUILD_TYPE="$bt" \
      -DMMORPG_BUILD_TESTS=ON \
      -DMMORPG_BUILD_BENCHMARKS=ON \
      -DVCPKG_MANIFEST_INSTALL=OFF \
      -DVCPKG_APPLOCAL_DEPS=OFF \
      -DCMAKE_CXX_COMPILER="C:/msys64/mingw64/bin/c++.exe" \
      -DCMAKE_C_COMPILER="C:/msys64/mingw64/bin/gcc.exe" \
      -DCMAKE_MAKE_PROGRAM="C:/msys64/mingw64/bin/ninja.exe" \
      || die "cmake configure 失败（离线，$bt）。禁止跳过，先修根因。"
  else
    cmake -S "$ROOT" -B "$dir" -G "$GENERATOR" \
      -DCMAKE_BUILD_TYPE="$bt" \
      -DCMAKE_TOOLCHAIN_FILE="$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" \
      -DMMORPG_BUILD_TESTS=ON \
      -DMMORPG_BUILD_BENCHMARKS=ON \
      || die "cmake configure 失败（$bt）。禁止跳过，先修根因。"
  fi
  ok "cmake configure 通过（$bt）"
}

# ---- 编译 ----------------------------------------------------------------
# 支持多目标：cmake_build <bt> <t1> [t2 ...]；缺省 all。
cmake_build() {
  local bt="${1:-$BUILD_TYPE}"; shift || true
  local targets=("${@:-all}")
  step "cmake build ($bt / ${targets[*]})"
  for t in "${targets[@]}"; do
    cmake --build "$BUILD_ROOT/$bt" --target "$t" -j "$PARALLEL" \
      || die "编译失败（$bt / $t）。禁止注释代码绕过，禁止 -k 忽略错误。"
  done
  ok "编译通过（$bt / ${targets[*]}）"
}

# ---- Debug + Release 双构建（基础类任务要求） ----------------------------
# MMO_BUILD_TARGET 可限定只编本任务相关目标（离线验收避免拉起整项目 vcpkg-only 依赖）。
cmake_build_both() {
  local tgts="${MMO_BUILD_TARGET:-all}"
  cmake_configure Debug;   cmake_build Debug $tgts
  cmake_configure Release; cmake_build Release $tgts
}

# ---- ctest ---------------------------------------------------------------
run_ctest() {
  local bt="${1:-$BUILD_TYPE}" pattern="$2" label="${3:-$2}"
  step "ctest -R '$pattern' ($label)"
  # 先确认过滤模式至少匹配到 1 个用例：ctest 在「零匹配」时打印 No tests were found!!!
  # 却返回退出码 0 —— 一旦模式与实际注册名不符（如真实名带点号 DataService.Redis 而写成
  # DataService_Redis），验收会「零用例假通过」。此处显式拦截，禁止以 0 用例视为通过。
  local matched
  matched="$( cd "$BUILD_ROOT/$bt" && "$CTEST_BIN" -N -R "$pattern" 2>/dev/null \
              | sed -nE 's/^Total Tests: *([0-9]+).*/\1/p' | tail -1 )"
  [ -n "$matched" ] || die "无法解析 ctest -N 输出（$label）：请检查 CTEST_BIN 与构建目录"
  [ "$matched" -gt 0 ] \
    || die "ctest 过滤 '$pattern' 未匹配到任何用例（0 个）。真实注册名可能带点号或命名不同，禁止以 0 用例视为通过。"
  ( cd "$BUILD_ROOT/$bt" && "$CTEST_BIN" --output-on-failure -R "$pattern" ) \
    || die "测试失败：$label（ctest -R '$pattern'）"
  ok "测试通过：$label（匹配 $matched 个用例）"
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

# ---- 端口已占用检查（真实实例类任务：期望实例已在线监听，如 Redis/MariaDB）----
# 与 require_free_port 语义相反；真实实例任务（§20.1）要求端口被实例占用而非空闲。
require_port_open() {
  local p="$1"
  if (exec 3<>"/dev/tcp/127.0.0.1/$p") 2>/dev/null; then
    exec 3>&- 2>/dev/null
    ok "端口已占用（实例在线）：$p"
  else
    die "端口 $p 未占用（期望有真实实例在监听，如 Redis/MariaDB）。先启动实例再验收。"
  fi
}

# ---- Godot 门禁（2026-09-14 新增，docs/client-spec-2.5d.md §10）----------
# 客户端走 Godot 路线后新增。Compatibility 渲染器只需 OpenGL 3.3 / D3D11，
# 不要求 Vulkan，因此门禁只校验可执行与版本号。
GODOT_EXPECTED_VERSION="${GODOT_EXPECTED_VERSION:-4.7.2}"
# 本机已解压核验的默认路径（4.7.2.stable.official.ed1daf0bf）
GODOT_DEFAULT_BIN="${GODOT_DEFAULT_BIN:-F:/AI/tools/godot/4.7.2/Godot_v4.7.2-stable_win64.exe}"
CLIENT_DIR="${CLIENT_DIR:-client}"

require_godot() {
  local godot_bin="${GODOT_BIN:-}"
  if [ -z "$godot_bin" ]; then
    if command -v godot >/dev/null 2>&1; then
      godot_bin="$(command -v godot)"
    elif [ -x "$GODOT_DEFAULT_BIN" ]; then
      godot_bin="$GODOT_DEFAULT_BIN"
    fi
  fi
  [ -n "$godot_bin" ] && [ -x "$godot_bin" ] \
    || die "require_godot：未找到 Godot 可执行文件（设置 GODOT_BIN 或安装 ${GODOT_EXPECTED_VERSION}）"
  local version
  version="$("$godot_bin" --version 2>/dev/null | head -n 1 || true)"
  [ -n "$version" ] || die "require_godot：无法从 $godot_bin 获取版本号"
  case "$version" in
    *"$GODOT_EXPECTED_VERSION"*)
      export GODOT_BIN="$godot_bin"
      ok "require_godot：OK $godot_bin ($version)"
      return 0
      ;;
    *)
      die "require_godot：期望 ${GODOT_EXPECTED_VERSION}，实际 '$version'（$godot_bin）"
      ;;
  esac
}

# 解析 GDScript / 校验工程可加载（不做运行期断言）
godot_check_only() {
  require_godot
  step "godot --headless --path $CLIENT_DIR --check-only"
  "$GODOT_BIN" --headless --path "$CLIENT_DIR" --check-only \
    || die "Godot 工程解析失败（$CLIENT_DIR）"
  ok "Godot 工程解析通过：$CLIENT_DIR"
}

# headless 单元测试：参数为 res:// 下的测试脚本
godot_run_tests() {
  local script="${1:-res://runtime/tests/test_client_core.gd}"
  require_godot
  step "godot --headless --path $CLIENT_DIR --script $script"
  "$GODOT_BIN" --headless --path "$CLIENT_DIR" --script "$script" \
    || die "Godot 单元测试失败：$script"
  ok "Godot 单元测试通过：$script"
}

# Windows 64 位导出（Compatibility 单档，不依赖 Vulkan）
godot_build_win64() {
  local out="${1:-build/client/cami_client.exe}"
  require_godot
  mkdir -p "$(dirname "$out")"
  step "godot 导出 Windows Desktop -> $out"
  "$GODOT_BIN" --headless --path "$CLIENT_DIR" \
    --export-release "Windows Desktop" "$out" \
    || die "Godot 导出失败：$out"
  ok "Godot 导出完成：$out"
}
