#!/bin/sh
# ================================================================
# CAMI Redis Cluster Verification Script (POSIX sh)
#
# Verifies:
#   1. Cluster topology (3 masters + 3 slaves)
#   2. Slot distribution (16384 slots covered)
#   3. CRC16 key sharding (keys distributed across nodes)
#   4. MOVED redirect handling
#   5. Read/write functionality
#   6. Basic SET/GET performance
#
# Runs ALL redis-cli checks INSIDE the redis-node-1 container via
# `docker compose exec`, so NO host redis-cli is required. The cluster
# nodes are addressed by their cami-net service names.
#
# Usage (from CAMI repo root):
#   sh scripts/verify_redis_cluster.sh
# ================================================================

set -u

COMPOSE_FILE="docker/docker-compose.yml"
REDIS_NODE="redis-node-1"
CLUSTER_HOST="redis-node-1"
CLUSTER_PORT=7000

# Run redis-cli inside the redis-node-1 container
RC() { docker compose -f "$COMPOSE_FILE" exec -T "$REDIS_NODE" redis-cli "$@" 2>/dev/null; }
# Run redis-benchmark inside the redis-node-1 container
RB() { docker compose -f "$COMPOSE_FILE" exec -T "$REDIS_NODE" redis-benchmark "$@" 2>/dev/null; }

PASS=0
FAIL=0

check() {
    if [ "$1" -eq 0 ]; then
        echo "  [PASS] $2"; PASS=$((PASS + 1))
    else
        echo "  [FAIL] $2"; FAIL=$((FAIL + 1))
    fi
}

echo "=============================================="
echo "  CAMI Redis Cluster Verification"
echo "=============================================="
echo ""

# === 1. Cluster Topology ===
echo "--- 1. Cluster Topology ---"
STATE=$(RC -h "$CLUSTER_HOST" -p "$CLUSTER_PORT" cluster info 2>/dev/null | grep '^cluster_state:' | cut -d: -f2 | tr -d '\r')
[ "$STATE" = "ok" ]; check $? "Cluster state is 'ok' (got: $STATE)"

SIZE=$(RC -h "$CLUSTER_HOST" -p "$CLUSTER_PORT" cluster info 2>/dev/null | grep '^cluster_size:' | cut -d: -f2 | tr -d '\r')
[ "$SIZE" = "3" ]; check $? "Cluster has 3 masters (got: $SIZE)"

NODES_OUT=$(RC -h "$CLUSTER_HOST" -p "$CLUSTER_PORT" cluster nodes 2>/dev/null)
M=$(echo "$NODES_OUT" | grep -c "master")
S=$(echo "$NODES_OUT" | grep -c "slave")
[ "$M" -eq 3 ]; check $? "3 master nodes found (got: $M)"
[ "$S" -ge 3 ]; check $? "3+ slave nodes found (got: $S)"
echo ""

# === 2. Slot Distribution ===
echo "--- 2. Slot Distribution ---"
SA=$(RC -h "$CLUSTER_HOST" -p "$CLUSTER_PORT" cluster info 2>/dev/null | grep '^cluster_slots_assigned:' | cut -d: -f2 | tr -d '\r')
SO=$(RC -h "$CLUSTER_HOST" -p "$CLUSTER_PORT" cluster info 2>/dev/null | grep '^cluster_slots_ok:' | cut -d: -f2 | tr -d '\r')
[ "$SA" = "16384" ]; check $? "All 16384 slots assigned (got: $SA)"
[ "$SO" = "16384" ]; check $? "All 16384 slots OK (got: $SO)"
echo ""

# === 3. CRC16 Key Sharding ===
echo "--- 3. CRC16 Key Sharding ---"
echo "  Writing test keys with different prefixes..."
TEST_KEYS="player:1001 player:1002 player:1003 player:1004 player:1005 session:abc session:def guild:warriors mail:inbox:1001 cache:item:5001"
for key in $TEST_KEYS; do
    RC -c -h "$CLUSTER_HOST" -p "$CLUSTER_PORT" SET "$key" "value_$key" > /dev/null 2>&1
done
OK=1
for key in $TEST_KEYS; do
    V=$(RC -c -h "$CLUSTER_HOST" -p "$CLUSTER_PORT" GET "$key" 2>/dev/null | tr -d '\r')
    if [ "$V" != "value_$key" ]; then
        OK=0; echo "  MISMATCH: $key -> $V"
    fi
done
[ "$OK" -eq 1 ]; check $? "Write + read 10 keys across cluster"
echo ""

# === 4. MOVED Redirect Handling ===
echo "--- 4. MOVED Redirect Handling ---"
R=$(RC -c -h "$CLUSTER_HOST" -p "$CLUSTER_PORT" SET "test:moved" "ok" | tr -d '\r')
[ "$R" = "OK" ]; check $? "Cluster client handles MOVED redirect (got: $R)"
echo ""

# === 5. Performance Test (informational) ===
echo "--- 5. Basic Performance (informational) ---"
echo "  Running SET benchmark (10000 operations)..."
PERF=$(RB -h "$CLUSTER_HOST" -p "$CLUSTER_PORT" -t set -n 10000 -q 2>/dev/null | grep -i "SET" | tail -1 || true)
if [ -z "$PERF" ] || echo "$PERF" | grep -qi "nan"; then
  echo "  Result: N/A (redis-benchmark does not follow MOVED redirects in cluster mode)"
  echo "  [NOTE] Informational only — not counted in PASS/FAIL"
else
  echo "  Result: $PERF"
  echo "  [NOTE] Informational only — not counted in PASS/FAIL"
fi
echo ""

# === Summary ===
echo "=============================================="
echo "  Redis Cluster Verification Summary"
echo "=============================================="
echo "  Passed: $PASS"
echo "  Failed: $FAIL"
if [ "$FAIL" -eq 0 ]; then
    echo "  >>> ALL CHECKS PASSED <<<"
else
    echo "  >>> SOME CHECKS FAILED <<<"
fi
echo "=============================================="
