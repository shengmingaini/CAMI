#!/usr/bin/env bash
# tools/chaos/gateway_crash.sh — TASK-038 Chaos：Gateway 硬崩溃
#
# 模拟 Gateway 进程崩溃（SIGKILL），验证：
#   - 已连接玩家连接中断被感知
#   - ControlService / 健康检查探测到网关缺失
#   - 新连接被健康网关接管（多副本无单点）
#
# 安全边界（混沌工程红线）：
#   1. 默认 dry-run，加 --yes 才真正发信号。
#   2. 必须显式给 --pid 或 --service（docker compose）/ --pod（k8s），不默认全杀。
#   3. 恢复断言：崩溃后 RECOVER_S 秒内，网关健康检查恢复（新副本已起）。
#
# 用法：
#   bash tools/chaos/gateway_crash.sh --pid 12345 --yes
#   bash tools/chaos/gateway_crash.sh --service gateway --yes     # docker compose
#   bash tools/chaos/gateway_crash.sh --pod gateway-abc --yes     # kubectl

set -euo pipefail
PID=""; SVC=""; POD=""; YES=0; SIGNAL=9; RECOVER_S=30
while [ $# -gt 0 ]; do case "$1" in
  --pid) shift; PID="$1";; --service) shift; SVC="$1";; --pod) shift; POD="$1";;
  --signal) shift; SIGNAL="$1";; --recover) shift; RECOVER_S="$1";;
  --yes) YES=1;; -h|--help) grep '^#' "$0"|sed 's/^# \{0,1\}//'; exit 0;;
  *) echo "未知参数：$1" >&2; exit 2;; esac; shift; done

probe() { curl -sf -o /dev/null "http://localhost:9001/healthz" 2>/dev/null; }

if [ -n "$PID" ]; then
  echo "== chaos: SIG$SIGNAL -> PID $PID =="; $YES || { echo "[dry-run] 加 --yes 执行"; exit 0; }
  kill -"$SIGNAL" "$PID" 2>/dev/null || echo "发送失败（进程可能已不存在）" >&2
elif [ -n "$SVC" ]; then
  echo "== chaos: docker compose stop $SVC =="; $YES || { echo "[dry-run] 加 --yes 执行"; exit 0; }
  docker compose stop "$SVC"
elif [ -n "$POD" ]; then
  echo "== chaos: kubectl delete pod $POD =="; $YES || { echo "[dry-run] 加 --yes 执行"; exit 0; }
  kubectl -n mmorpg delete pod "$POD" --wait=false
else
  echo "必须指定 --pid / --service / --pod 之一" >&2; exit 4
fi

if [ "$YES" = "1" ]; then
  echo "等待恢复（$RECOVER_S 秒）..."; sleep "$RECOVER_S"
  if probe; then echo "RECOVERY: PASS (gateway 健康检查恢复)"; else echo "RECOVERY: FAIL (健康检查未恢复)"; exit 1; fi
fi
exit 0
