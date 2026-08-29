#!/usr/bin/env bash
# task-done.sh — 将任务标记为 DONE（唯一被允许的 STATUS 变更入口）
# 用法：bash scripts/task-done.sh TASK-001
# 行为：
#   1) 校验任务文件存在且当前 STATUS 为 PENDING（禁止重复 / 逆向流转）
#   2) 必须先通过对应验收脚本 scripts/verify/task-NNN.sh（退出码 0）
#   3) 将 front matter 与正文的 STATUS 由 PENDING 改为 DONE
#   4) 不修改任何其它内容（接口契约冻结）
set -euo pipefail

# 仓库根（脚本位于 scripts/，上两级）
MMORPG_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
if command -v cygpath >/dev/null 2>&1; then
  MMORPG_ROOT="$(cygpath -m "$MMORPG_ROOT")"
fi
export MMORPG_ROOT

# 验收脚本所需的路径覆盖（本仓库任务位于 mmorpg_tasks/tasks；cmake 需要 Windows 路径）
export MMO_PROJECT_ROOT="$MMORPG_ROOT"
export MMO_TASKS_DIR="$MMORPG_ROOT/mmorpg_tasks/tasks"
export VCPKG_ROOT="$(cygpath -m "${VCPKG_ROOT:-/c/vcpkg}")"
export GENERATOR="${GENERATOR:-Ninja}"

TID="${1:-}"
if [[ -z "$TID" ]]; then
  echo "用法: bash scripts/task-done.sh TASK-001" >&2
  exit 2
fi

TASK_FILE="$MMORPG_ROOT/mmorpg_tasks/tasks/$TID.md"
[[ -f "$TASK_FILE" ]] || { echo "任务文件不存在: $TASK_FILE" >&2; exit 3; }

# 1) 当前 STATUS 必须为 PENDING
cur="$(grep -m1 '^STATUS:' "$TASK_FILE" | sed -E 's/^STATUS:[[:space:]]*//; s/[*]//g; s/[[:space:]]//g; s/\r//g')"
if [[ "$cur" == "DONE" ]]; then
  echo "$TID 已经是 DONE，禁止重复流转" >&2
  exit 4
fi
if [[ "$cur" != "PENDING" ]]; then
  echo "$TID 当前 STATUS='$cur'，仅允许从 PENDING 流转到 DONE" >&2
  exit 4
fi
echo "$TID 当前 STATUS=PENDING，允许流转"

# 2) 必须先通过验收脚本
VERIFY="$MMORPG_ROOT/scripts/verify/$TID.sh"
if [[ -f "$VERIFY" ]]; then
  echo "运行验收脚本: $VERIFY"
  if bash "$VERIFY"; then
    echo "$TID 验收脚本通过"
  else
    rc=$?
    echo "$TID 验收脚本未通过（exit=$rc），禁止标记 DONE" >&2
    exit 5
  fi
else
  echo "未找到验收脚本 $VERIFY，跳过自动验收（请人工确认）"
fi

# 3) 流转 STATUS: PENDING -> DONE（front matter 与正文表格）
tmp="$(mktemp)"
awk '
  /^STATUS:/ { sub(/PENDING/, "DONE"); print; next }
  /\| STATUS \|/ { sub(/\*\*PENDING\*\*/, "**DONE**"); print; next }
  { print }
' "$TASK_FILE" > "$tmp"
mv "$tmp" "$TASK_FILE"
echo "$TID STATUS 已流转为 DONE"

echo "下一步：按 TASK 文件 §25 提交（Conventional Commits，scope=模块，正文含实测数字）"
