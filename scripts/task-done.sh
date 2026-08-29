#!/usr/bin/env bash
# task-done.sh — 将任务标记为 DONE（唯一被允许的 STATUS 变更入口）
# 用法：bash scripts/task-done.sh TASK-000
# 行为：
#   1) 校验任务文件存在且当前 STATUS 为 PENDING（禁止重复 / 逆向流转）
#   2) 必须先通过对应验收脚本 scripts/verify/task-NNN.sh（退出码 0）
#   3) 将 front matter 与正文的 STATUS 由 PENDING 改为 DONE
#   4) 不修改任何其它内容（接口契约冻结）
set -euo pipefail
source "$(dirname "$0")/verify/_common.sh"

TID="${1:-}"
if [[ -z "$TID" ]]; then
  echo "用法: bash scripts/task-done.sh TASK-000" >&2
  exit 2
fi

TASK_FILE="$MMORPG_ROOT/mmorpg_tasks/tasks/$TID.md"
if [[ ! -f "$TASK_FILE" ]]; then
  fail "任务文件不存在: $TASK_FILE"; exit 3
fi

# 1) 当前 STATUS 必须为 PENDING
cur="$(grep -m1 '^STATUS:' "$TASK_FILE" | sed 's/STATUS:[[:space:]]*//; s/[*]//g' | tr -d '\r')"
if [[ "$cur" == "DONE" ]]; then
  fail "$TID 已经是 DONE，禁止重复流转"; exit 4
fi
if [[ "$cur" != "PENDING" ]]; then
  fail "$TID 当前 STATUS='$cur'，仅允许从 PENDING 流转到 DONE"; exit 4
fi
ok "$TID 当前 STATUS=PENDING，允许流转"

# 2) 必须先通过验收脚本
VERIFY="$MMORPG_ROOT/scripts/verify/$TID.sh"
if [[ -x "$VERIFY" || -f "$VERIFY" ]]; then
  log "运行验收脚本: $VERIFY"
  bash "$VERIFY"
  rc=$?
  if [[ $rc -ne 0 ]]; then fail "$TID 验收脚本未通过（exit=$rc），禁止标记 DONE"; exit 5; fi
  ok "验收脚本通过"
else
  log "未找到验收脚本 $VERIFY，跳过自动验收（请人工确认）"
fi

# 3) 流转 STATUS: PENDING -> DONE（front matter 与正文表格）
#    front matter: STATUS: PENDING
#    body table:  | STATUS | **PENDING** |
tmp="$(mktemp)"
awk '
  /^STATUS:/ { sub(/PENDING/, "DONE"); print; next }
  /\| STATUS \|/ { sub(/\*\*PENDING\*\*/, "**DONE**"); print; next }
  { print }
' "$TASK_FILE" > "$tmp"
mv "$tmp" "$TASK_FILE"
ok "$TID STATUS 已流转为 DONE"

log "下一步：按 TASK 文件 §25 提交（Conventional Commits，scope=模块，正文含实测数字）"
