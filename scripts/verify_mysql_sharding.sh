#!/bin/bash
# ================================================================
# CAMI MySQL + ShardingSphere Verification Script
#
# Verifies:
#   1. ShardingSphere Proxy connectivity
#   2. Sharded table existence on both shards
#   3. PlayerID-based sharding (MOD 2)
#   4. Cross-shard data distribution
#   5. Query routing (point query)
#   6. Basic CRUD operations through proxy
#   7. Cross-reference: currency table sharding
#
# Runs ALL mysql checks INSIDE the mysql-shard-1 container via
# `docker compose exec`, so NO host mysql client is required.
# Connects to the ShardingSphere Proxy (shardingsphere-proxy:3307)
# and to each shard directly over the cami-net network.
#
# Prerequisites:
#   - Docker compose services running
#     (mysql-shard-1, mysql-shard-2, shardingsphere-proxy)
#   - init-shard.sql applied to both shards
#
# Usage (from CAMI repo root):
#   bash scripts/verify_mysql_sharding.sh
# ================================================================

set -uo pipefail

COMPOSE_FILE="docker/docker-compose.yml"
MYSQL_NODE="mysql-shard-1"          # we exec into this container (has mysql client)

SS_HOST="shardingsphere-proxy"; SS_PORT=3307
SHARD1_HOST="127.0.0.1";      SHARD1_PORT=3306   # mysql-shard-1 itself
SHARD2_HOST="mysql-shard-2";   SHARD2_PORT=3306   # reachable via cami-net DNS

SS_USER="root";          SS_PASS="root"
SHARD_USER="root";        SHARD_PASS="cami_dev_2026"

MY_SS()  { docker compose -f "$COMPOSE_FILE" exec -T "$MYSQL_NODE" mysql -h "$SS_HOST"    -P "$SS_PORT"    -u "$SS_USER"    -p"$SS_PASS"    -N -D cami_db -e "$1" 2>/dev/null; }
MY_SH1() { docker compose -f "$COMPOSE_FILE" exec -T "$MYSQL_NODE" mysql -h "$SHARD1_HOST" -P "$SHARD1_PORT" -u "$SHARD_USER" -p"$SHARD_PASS" -N -e "$1" 2>/dev/null; }
MY_SH2() { docker compose -f "$COMPOSE_FILE" exec -T "$MYSQL_NODE" mysql -h "$SHARD2_HOST" -P "$SHARD2_PORT" -u "$SHARD_USER" -p"$SHARD_PASS" -N -e "$1" 2>/dev/null; }

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
echo "  CAMI MySQL + ShardingSphere Verification"
echo "=============================================="
echo ""

# === 1. ShardingSphere Proxy Connectivity ===
echo "--- 1. ShardingSphere Proxy Connectivity ---"
RESULT=$(MY_SS "SELECT 1" 2>/dev/null || echo "FAIL")
[ "$RESULT" = "1" ]; check $? "ShardingSphere proxy reachable (shardingsphere-proxy:$SS_PORT)"
echo ""

# === 2. Sharded Table Existence ===
echo "--- 2. Sharded Table Existence ---"
SHARD1_TABLES=$(MY_SH1 "SHOW TABLES FROM cami_shard_1" 2>/dev/null || echo "")
SHARD2_TABLES=$(MY_SH2 "SHOW TABLES FROM cami_shard_2" 2>/dev/null || echo "")
echo "  Shard 1 tables: $(echo $SHARD1_TABLES | tr '\n' ' ')"
echo "  Shard 2 tables: $(echo $SHARD2_TABLES | tr '\n' ' ')"
echo "$SHARD1_TABLES" | grep -q "player_base"; check $? "player_base exists on shard 1"
echo "$SHARD2_TABLES" | grep -q "player_base"; check $? "player_base exists on shard 2"
echo ""

# === 3. Clean Up Previous Test Data ===
echo "--- 3. Preparing Clean Test Data ---"
MY_SH1 "DELETE FROM cami_shard_1.player_base WHERE player_id BETWEEN 900001 AND 900010" 2>/dev/null || true
MY_SH2 "DELETE FROM cami_shard_2.player_base WHERE player_id BETWEEN 900001 AND 900010" 2>/dev/null || true
# Currency fixtures must also be cleared up-front, otherwise a re-run hits a
# duplicate-key error on the step-7 INSERT (the end-of-script proxy DELETE is
# not guaranteed to have run on a prior interrupted invocation).
MY_SH1 "DELETE FROM cami_shard_1.player_currency WHERE player_id BETWEEN 900001 AND 900010" 2>/dev/null || true
MY_SH2 "DELETE FROM cami_shard_2.player_currency WHERE player_id BETWEEN 900001 AND 900010" 2>/dev/null || true
echo "  Cleaned up previous test data."
echo ""

# === 4. PlayerID-based Sharding (MOD 2) ===
echo "--- 4. PlayerID-based Sharding (MOD 2) ---"
echo "  Inserting 10 test players (IDs 900001-900010) through ShardingSphere..."
OK=0
i=1
while [ "$i" -le 10 ]; do
    PID=$((900000 + i))
    NAME="test_player_$i"
    if MY_SS "INSERT INTO player_base (player_id, account_id, name, class_id, level, exp) VALUES ($PID, $PID, '$NAME', 1, 1, 0)" 2>/dev/null; then
        OK=$((OK + 1))
    fi
    i=$((i + 1))
done
[ "$OK" -eq 10 ]; check $? "Insert 10 players through ShardingSphere proxy"

SHARD1_COUNT=$(MY_SH1 "SELECT COUNT(*) FROM cami_shard_1.player_base WHERE player_id BETWEEN 900001 AND 900010" 2>/dev/null || echo "0")
SHARD2_COUNT=$(MY_SH2 "SELECT COUNT(*) FROM cami_shard_2.player_base WHERE player_id BETWEEN 900001 AND 900010" 2>/dev/null || echo "0")
echo "    Shard 1 (ds_0): $SHARD1_COUNT players"
echo "    Shard 2 (ds_1): $SHARD2_COUNT players"
echo "    Total:          $((SHARD1_COUNT + SHARD2_COUNT)) players"

EXPECTED_SHARD1=5   # even IDs: 900002,900004,900006,900008,900010
EXPECTED_SHARD2=5   # odd IDs:  900001,900003,900005,900007,900009
[ "$SHARD1_COUNT" = "$EXPECTED_SHARD1" ]; check $? "Shard 1 has $EXPECTED_SHARD1 players (even IDs) [got: $SHARD1_COUNT]"
[ "$SHARD2_COUNT" = "$EXPECTED_SHARD2" ]; check $? "Shard 2 has $EXPECTED_SHARD2 players (odd IDs) [got: $SHARD2_COUNT]"
echo ""

# === 5. Point Query Routing ===
echo "--- 5. Point Query Routing ---"
PID=900005
RESULT=$(MY_SS "SELECT name FROM player_base WHERE player_id = $PID" 2>/dev/null || echo "NOT_FOUND")
echo "  Query player_id=$PID through proxy: name='$RESULT'"
[ "$RESULT" = "test_player_5" ]; check $? "Point query returns correct data"

SHARD_CHECK=$(MY_SH2 "SELECT name FROM cami_shard_2.player_base WHERE player_id = $PID" 2>/dev/null || echo "NOT_ON_SHARD2")
[ "$SHARD_CHECK" = "test_player_5" ]; check $? "player_id=$PID confirmed on shard 2 (odd -> ds_1)"
echo ""

# === 6. CRUD Operations ===
echo "--- 6. CRUD Operations ---"
MY_SS "UPDATE player_base SET level = 99, exp = 999999 WHERE player_id = 900001" 2>/dev/null
[ $? -eq 0 ]; check $? "UPDATE player 900001"

UPDATED_LEVEL=$(MY_SS "SELECT level FROM player_base WHERE player_id = 900001" 2>/dev/null || echo "0")
[ "$UPDATED_LEVEL" = "99" ]; check $? "UPDATE verified (level=99, got=$UPDATED_LEVEL)"

MY_SS "DELETE FROM player_base WHERE player_id = 900010" 2>/dev/null
[ $? -eq 0 ]; check $? "DELETE player 900010"

DELETED_CHECK=$(MY_SS "SELECT COUNT(*) FROM player_base WHERE player_id = 900010" 2>/dev/null || echo "1")
[ "$DELETED_CHECK" = "0" ]; check $? "DELETE verified (count=0)"
echo ""

# === 7. Cross-Reference: Currency Table ===
echo "--- 7. Currency Table Sharding ---"
MY_SS "INSERT INTO player_currency (player_id, currency_type, amount) VALUES (900001, 1, 10000)" 2>/dev/null
MY_SS "INSERT INTO player_currency (player_id, currency_type, amount) VALUES (900002, 1, 20000)" 2>/dev/null
[ $? -eq 0 ]; check $? "Insert currency for 900001 (odd->ds_1) and 900002 (even->ds_0)"

CUR1=$(MY_SH1 "SELECT amount FROM cami_shard_1.player_currency WHERE player_id = 900002" 2>/dev/null || echo "NOT_FOUND")
CUR2=$(MY_SH2 "SELECT amount FROM cami_shard_2.player_currency WHERE player_id = 900001" 2>/dev/null || echo "NOT_FOUND")
echo "  player 900002 currency on shard 1: $CUR1"
echo "  player 900001 currency on shard 2: $CUR2"
[ "$CUR1" = "20000" ]; check $? "Currency for even-ID player on shard 1"
[ "$CUR2" = "10000" ]; check $? "Currency for odd-ID player on shard 2"
echo ""

# === Cleanup ===
echo "--- Cleanup ---"
MY_SS "DELETE FROM player_base WHERE player_id BETWEEN 900001 AND 900010" 2>/dev/null
MY_SS "DELETE FROM player_currency WHERE player_id BETWEEN 900001 AND 900010" 2>/dev/null
echo "  Test data cleaned up."
echo ""

# === Summary ===
echo "=============================================="
echo "  MySQL + ShardingSphere Verification Summary"
echo "=============================================="
echo "  Passed: $PASS"
echo "  Failed: $FAIL"
if [ "$FAIL" -eq 0 ]; then
    echo "  >>> ALL CHECKS PASSED <<<"
else
    echo "  >>> SOME CHECKS FAILED <<<"
fi
echo "=============================================="
