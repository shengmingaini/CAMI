#!/usr/bin/env bash
# ============================================================================
# benchmark/sysbench/run-8shard.sh — 8 分库并行压测 runner (Week4 D6-T4)
# ----------------------------------------------------------------------------
# 对 8 个分片主库 (mysql-shard-0 .. mysql-shard-7) 各起一路 sysbench oltp_insert,
# 汇总 TPS 给出集群写入容量基线。
#
# 前置:
#   - Docker compose 栈已起 (docker/docker-compose.yml), 含 8 主 8 从 + SS 代理。
#   - 宿主机已装 sysbench (apt install sysbench / brew install sysbench)。
#   - 单分片实测基线: 9784 TPS (容器内单连接直插, 见 docs/database/shard-tps-bench.md)。
#
# 注意 (沙箱坑, 见 docs/database/shard-tps-bench.md §4):
#   - 不要用 `docker compose exec` 并发跑导入 —— 那是编排开销假象, 非 DB 上限。
#   - 这里每路 sysbench 直接连各自分片主库容器 IP/端口, 由宿主机 sysbench 驱动。
#     若宿主机 NAT 隔离连不上容器端口, 改为在容器内跑 (见下方 IN_CONTAINER 段)。
# ============================================================================
set -euo pipefail

# ---- 可调参数 ----
SHARDS=${SHARDS:-8}
THREADS=${THREADS:-16}
TABLES=${TABLES:-1}
TABLE_SIZE=${TABLE_SIZE:-200000}
TIME=${TIME:-60}            # 每路压测时长(秒)
DB_USER=${DB_USER:-root}
DB_PASS=${DB_PASS:-cami_dev_2026}
# 分片主库地址: 默认走宿主机映射端口 (3306 + i 偏移, 按 docker-compose 端口映射调整)
# 若连不上, 改用容器服务名/容器 IP (见 IN_CONTAINER)。
BASE_PORT=${BASE_PORT:-33060}   # mysql-shard-0 -> 33060, shard-1 -> 33061 ...
SS_PROXY=${SS_PROXY:-127.0.0.1:3309}   # ShardingSphere 代理 (读写分离/路由)

OUT_DIR=${OUT_DIR:-benchmark/sysbench/results}
mkdir -p "$OUT_DIR"

echo "=== 8 分库并行 sysbench oltp_insert ==="
echo "shards=$SHARDS threads=$THREADS tables=$TABLES table_size=$TABLE_SIZE time=${TIME}s"

pids=()
for ((i=0; i<SHARDS; i++)); do
  port=$((BASE_PORT + i))
  db="cami_shard_$i"
  log="$OUT_DIR/shard_$i.log"
  sysbench oltp_insert \
    --db-driver=mysql \
    --mysql-host=127.0.0.1 --mysql-port=$port \
    --mysql-user=$DB_USER --mysql-password=$DB_PASS \
    --mysql-db=$db \
    --tables=$TABLES --table-size=$TABLE_SIZE \
    --threads=$THREADS --time=$TIME --report-interval=10 \
    run > "$log" 2>&1 &
  pids+=($!)
  echo "  launched shard_$i -> 127.0.0.1:$port/$db (log=$log)"
done

# 等待全部完成
total_tps=0
for ((i=0; i<SHARDS; i++)); do
  wait ${pids[$i]}
  # 提取 "transactions:" 行的 TPS (sysbench 输出: "transactions: 12345 (205.75 per sec.)")
  tps=$(grep -E "transactions:" "$OUT_DIR/shard_$i.log" | sed -E 's/.*\(([0-9.]+) per sec\.\)/\1/' | head -1)
  echo "  shard_$i TPS = ${tps:-N/A}"
  # 累加 (数值解析失败则跳过)
  if [[ "$tps" =~ ^[0-9.]+$ ]]; then
    total_tps=$(awk "BEGIN{print $total_tps + $tps}")
  fi
done

echo "============================================"
echo "集群写入容量基线 (8 分片并行) = $total_tps TPS"
echo "单分片基线 (实测)            = 9784 TPS"
echo "理论 8×单分片                = $((8*9784)) TPS"
echo "结果写入: $OUT_DIR/cluster_summary.txt"
echo "$total_tps" > "$OUT_DIR/cluster_summary.txt"
echo "============================================"

# ---- IN_CONTAINER 备选 (宿主机 NAT 隔离时) ----
# 若宿主机连不上容器端口, 在任一 mysql 容器里跑单路, 或每分片起一个容器进程:
# for i in 0..7; do
#   docker compose -f docker/docker-compose.yml exec -d mysql-shard-$i \
#     bash -c "sysbench oltp_insert --db-driver=mysql --mysql-socket=/var/run/mysqld/mysqld.sock \
#       --mysql-user=root --mysql-password=cami_dev_2026 --mysql-db=cami_shard_$i \
#       --tables=1 --table-size=200000 --threads=16 --time=60 run" \
#       > results/shard_$i.log 2>&1
# done
# (容器内需预装 sysbench: docker exec mysql-shard-0 apt-get update && apt-get install -y sysbench)
