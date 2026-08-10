#!/bin/sh
# ================================================================
# CAMI Redis Cluster Initialization Script (POSIX sh)
#
# Creates a 3-master + 3-slave Redis Cluster from 6 standalone nodes.
#
# IMPORTANT (Docker / cami-net): We announce each node's SERVICE NAME
# (redis-node-N) via `cluster-announce-hostname`. On a Docker user-defined
# network (cami-net) every container can resolve the other services' names
# via DNS, so the nodes' gossip bus reaches the correct container.
# (Redis rejects `cluster-announce-ip` with a non-IP like host.docker.internal.)
#
# Run it INSIDE the redis-node-1 container on the cami-net network:
#   docker compose -f docker/docker-compose.yml exec redis-node-1 sh /scripts/init-cluster.sh
# ================================================================

set -e

CLUSTER_HOST="redis-node-1"
CLUSTER_PORT=7000
NODES="redis-node-1:7000 redis-node-2:7001 redis-node-3:7002 redis-node-4:7003 redis-node-5:7004 redis-node-6:7005"

echo "=============================================="
echo "  CAMI Redis Cluster Initialization"
echo "=============================================="
echo ""

# Step 1: Wait for all nodes to be ready
echo "[1/4] Waiting for all Redis nodes to be ready..."
for node in $NODES; do
    h=$(echo "$node" | cut -d: -f1)
    p=$(echo "$node" | cut -d: -f2)
    echo -n "  Waiting for $node ... "
    ok=0
    i=0
    while [ "$i" -lt 30 ]; do
        if redis-cli -h "$h" -p "$p" ping 2>/dev/null | grep -q PONG; then
            ok=1
            break
        fi
        i=$((i + 1))
        sleep 1
    done
    if [ "$ok" -eq 1 ]; then
        echo "OK"
    else
        echo "FAILED"
        echo "ERROR: Node $node is not responding."
        exit 1
    fi
done
echo ""

# Step 2: Check if cluster is already initialized
echo "[2/4] Checking existing cluster state..."
if redis-cli -h "$CLUSTER_HOST" -p "$CLUSTER_PORT" cluster info 2>/dev/null | grep -q "cluster_state:ok"; then
    echo "  Cluster already initialized. Skipping creation."
    echo ""
else
    echo "  Cluster not initialized. Proceeding with creation."
    echo ""
    # Step 3: Create the cluster
    echo "[3/4] Creating Redis Cluster (3 masters + 3 slaves)..."
    echo ""
    redis-cli --cluster create $NODES --cluster-replicas 1 --cluster-yes
    echo ""
    echo "  Cluster creation completed."
    echo ""
fi

# Step 4: Verify cluster
echo "[4/4] Verifying cluster..."
# Wait for the cluster to stabilize: replicas may still be handshaking
# right after `cluster create` returns, which would show cluster_state:fail.
i=0
while [ "$i" -lt 30 ]; do
    ST=$(redis-cli -h "$CLUSTER_HOST" -p "$CLUSTER_PORT" cluster info 2>/dev/null | grep '^cluster_state:' | cut -d: -f2 | tr -d '\r')
    if [ "$ST" = "ok" ]; then
        break
    fi
    i=$((i + 1))
    sleep 1
done
echo ""
echo "--- Cluster Info ---"
redis-cli -h "$CLUSTER_HOST" -p "$CLUSTER_PORT" cluster info 2>/dev/null | grep -E "cluster_state|cluster_slots_assigned|cluster_slots_ok|cluster_known_nodes|cluster_size"
echo ""
echo "--- Cluster Nodes ---"
redis-cli -h "$CLUSTER_HOST" -p "$CLUSTER_PORT" cluster nodes 2>/dev/null
echo ""

M=$(redis-cli -h "$CLUSTER_HOST" -p "$CLUSTER_PORT" cluster nodes 2>/dev/null | grep -c "master")
S=$(redis-cli -h "$CLUSTER_HOST" -p "$CLUSTER_PORT" cluster nodes 2>/dev/null | grep -c "slave")

echo "  Masters: $M (expected: 3)"
echo "  Slaves:  $S (expected: 3)"
echo ""

if [ "$M" -eq 3 ] && [ "$S" -ge 3 ]; then
    echo "=============================================="
    echo "  >>> Redis Cluster initialized successfully <<<"
    echo "=============================================="
    echo ""
    echo "  Connect with (from a host on cami-net / via mapped ports):"
    echo "    redis-cli -c -h redis-node-1 -p 7000"
    echo ""
else
    echo "=============================================="
    echo "  >>> WARNING: Cluster node count mismatch <<<"
    echo "=============================================="
    exit 1
fi
