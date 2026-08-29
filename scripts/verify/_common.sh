#!/usr/bin/env bash
# _common.sh — TASK 验收公共函数（被 scripts/verify/task-NNN.sh 与 scripts/task-done.sh 引用）
# 使用方式：source "$(dirname "$0")/_common.sh"
# 本文件只定义函数，不执行任何副作用。
set -euo pipefail

# 把 MinGW MSYS2 工具链加入 PATH（git-bash 下 g++/cmake 可能不在默认 PATH，否则会静默失败）
export PATH="/c/msys64/mingw64/bin:/c/msys64/bin:$PATH"

MMORPG_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
# MSYS2 的 cmake 拒绝 POSIX 风格 -S/-B 路径（/f/...），需转成 Windows mixed 路径（F:/...）
if command -v cygpath >/dev/null 2>&1; then
  MMORPG_ROOT="$(cygpath -m "$MMORPG_ROOT")"
fi
export MMORPG_ROOT

# vcpkg 根目录同样转成 Windows 路径，供 toolchain file 使用
if [[ -n "${VCPKG_ROOT:-}" ]] && command -v cygpath >/dev/null 2>&1; then
  VCPKG_ROOT="$(cygpath -m "$VCPKG_ROOT")"
fi
export VCPKG_ROOT

log()  { printf '[verify] %s\n' "$*"; }
ok()   { printf '[ OK ]   %s\n' "$*"; }
fail() { printf '[FAIL]  %s\n' "$*" >&2; }

# 要求某个可执行文件存在
require_tool() {
  local t="$1"
  if ! command -v "$t" >/dev/null 2>&1; then
    fail "缺少工具: $t"; return 1
  fi
  ok "工具可用: $t ($($t --version 2>&1 | head -1))"
}

# 要求文件存在且非空
check_file() {
  local f="$1"
  if [[ ! -s "$f" ]]; then
    fail "缺失或为空: $f"; return 1
  fi
  ok "存在且非空: $f"
}

# 静态红线扫描：在目录 $1 中禁止出现正则 $2
redline_scan() {
  local dir="$1" re="$2" label="$3"
  if [[ ! -d "$dir" ]]; then ok "跳过(目录不存在): $dir"; return 0; fi
  if grep -rEn "$re" "$dir" >/dev/null 2>&1; then
    fail "红线违例[$label]: 在 $dir 发现匹配 '$re'"; return 1
  fi
  ok "红线通过[$label]: $dir 无 '$re'"
}

# cmake configure + build（单构建类型）。参数: <Release|Debug>
cmake_build() {
  local type="$1"
  local build_dir="$MMORPG_ROOT/build/$type"
  log "configure ($type)..."
  cmake -S "$MMORPG_ROOT" -B "$build_dir" -DCMAKE_BUILD_TYPE="$type" \
        -DCMAKE_TOOLCHAIN_FILE="${VCPKG_ROOT:-$MMORPG_ROOT/vcpkg}/scripts/buildsystems/vcpkg.cmake" \
    || { fail "cmake configure ($type) 失败"; return 1; }
  log "build ($type)..."
  cmake --build "$build_dir" -j || { fail "cmake build ($type) 失败"; return 1; }
  ok "configure + build ($type) 通过"
}
