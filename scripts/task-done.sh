#!/usr/bin/env bash
# ---------------------------------------------------------------------------
# 把指定任务标记为 DONE（唯一允许改写 STATUS 的入口）
#
# 用法：
#   bash scripts/task-done.sh TASK-013              # 只改 STATUS（需已自行跑过验收）
#   bash scripts/task-done.sh TASK-013 --verify     # 先跑验收脚本，通过后再改 STATUS
#   bash scripts/task-done.sh TASK-013 --reopen     # 回退为 PENDING（打回时）
#
# 红线：
#   1. 本脚本不推送 Git、不联网、不改任何业务代码。
#   2. 只允许 PENDING/BLOCKED → DONE，或 DONE → PENDING（--reopen）。
#   3. 依赖本任务的下游任务不得被本脚本触碰。
# ---------------------------------------------------------------------------
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
C_RED='\033[0;31m'; C_GRN='\033[0;32m'; C_YEL='\033[0;33m'; C_DIM='\033[2m'; C_OFF='\033[0m'
ok()   { printf "%b  [OK] %s%b\n"   "$C_GRN" "$*" "$C_OFF"; }
bad()  { printf "%b  [FAIL] %s%b\n" "$C_RED" "$*" "$C_OFF" >&2; }
info() { printf "%b  %s%b\n"        "$C_DIM" "$*" "$C_OFF"; }
die()  { bad "$*"; exit 1; }

TID="${1:-}"
[ -n "$TID" ] || die "用法：bash scripts/task-done.sh TASK-013 [--verify|--reopen]"
echo "$TID" | grep -qE '^TASK-[0-9]{3}$' || die "任务号格式错误：$TID（期望 TASK-013）"

MODE="done"
for a in "${@:2}"; do
  case "$a" in
    --verify) MODE="verify" ;;
    --reopen) MODE="reopen" ;;
    *) die "未知参数：$a（只支持 --verify / --reopen）" ;;
  esac
done

NUM="${TID#TASK-}"
MD="$ROOT/tasks/TASK-$NUM.md"
SH="$ROOT/scripts/verify/task-$NUM.sh"

[ -f "$MD" ] || die "任务文件不存在：$MD"
[ -f "$SH" ] || die "验收脚本不存在：$SH（先跑 python tools/gen/build_tasks.py 生成）"

cur="$(grep -m1 -E '^STATUS:' "$MD" || echo 'STATUS: 未知')"
cur="$(echo "$cur" | sed 's/^STATUS:[[:space:]]*//')"
info "任务：$TID"
info "当前 STATUS：$cur"

case "$MODE" in
  verify)
    info "先执行验收脚本（这是唯一可信路径，CI 不算）"
    bash "$SH" || die "验收脚本失败，$TID 不允许标记 DONE"
    ;;
  reopen)
    [ "$cur" = "DONE" ] || die "只有 DONE 状态可以回退，当前为 $cur"
    sed -i.bak "s/^STATUS:.*$/STATUS: PENDING/" "$MD" && rm -f "$MD.bak"
    ok "$TID → PENDING（已打回，请修复后重跑验收脚本）"
    exit 0
    ;;
  done)
    [ "$cur" = "PENDING" ] || [ "$cur" = "BLOCKED" ] \
      || die "当前 STATUS=$cur，只有 PENDING/BLOCKED 可流转到 DONE"
    ;;
esac

[ "$cur" = "PENDING" ] || [ "$cur" = "BLOCKED" ] \
  || die "当前 STATUS=$cur，只有 PENDING/BLOCKED 可流转到 DONE"

# 前置依赖必须已 DONE（双保险，验收脚本里也查一次）
deps="$(grep -m1 -E '^DEPENDENCIES:' "$MD" || echo '')"
deps="$(echo "$deps" | sed 's/^DEPENDENCIES:[[:space:]]*//')"
if [ -n "$deps" ] && [ "$deps" != "无" ]; then
  for d in $(echo "$deps" | tr -d ',' ); do
    n="${d#TASK-}"
    f="$ROOT/tasks/TASK-$n.md"
    [ -f "$f" ] || die "前置任务文件缺失：$f"
    grep -qE '^STATUS:[[:space:]]*DONE[[:space:]]*$' "$f" \
      || die "前置任务 $d 尚未 DONE，禁止把 $TID 标记为 DONE"
    ok "前置依赖 $d = DONE"
  done
fi

TODAY="$(date +%F)"
sed -i.bak "s/^STATUS:.*$/STATUS: DONE/" "$MD" && rm -f "$MD.bak"
grep -q "^DONE-DATE:" "$MD" \
  && sed -i.bak "s/^DONE-DATE:.*$/DONE-DATE: $TODAY/" "$MD" \
  || sed -i.bak "s/^STATUS: DONE$/STATUS: DONE\nDONE-DATE: $TODAY/" "$MD"
rm -f "$MD.bak"

ok "$TID → DONE（$TODAY）"
info "下一步：git add -A && git commit（Conventional Commits，正文必须贴实测数字）"
info "        git push git@github.com:22:shengmingaini/CAMI.git main"
