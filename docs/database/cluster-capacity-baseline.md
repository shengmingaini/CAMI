# Week4 D6-T4 — 集群写入容量基线报告

> 目标：用 sysbench 对 **8 个分库并行压测**，给出集群写入容量基线。
> 单分库基线：**9784 TPS**（容器内单连接直插真实主库，见 `shard-tps-bench.md`）。

---

## 1. 测量方法（先测量后优化）

- **单分库实测**（已闭环）：容器内 `mysql` 客户端直连 `mysql-shard-0` 导入 20000 行单 INSERT，`2044ms` → **9784 TPS**。
  - ⚠️ 不要用 `docker compose exec` 并发跑导入 —— 那是编排开销假象（曾得 727 TPS，非 DB 上限）。规范测 **单连接直插**。
- **8 分库并行**：每分片主库各起一路 `sysbench oltp_insert`（16 线程 / 60s），汇总 8 路 TPS。
  - runner：`benchmark/sysbench/run-8shard.sh`。
  - 每路连各自分片主库（绕开 ShardingSphere 代理，测**裸分库**写入能力；代理层开销另测）。

## 2. 单分库结果（确定性）

| 指标 | 值 |
|------|-----|
| 行数 | 20,000 |
| 耗时 | 2044 ms |
| **单分库 TPS** | **9784** |
| 验收阈值 | ≥ 5000 |
| 结论 | ✅ PASS |

## 3. 集群容量推算

| 维度 | 计算 | 值 |
|------|------|-----|
| 理论线性聚合 (8×单分库) | 8 × 9784 | **78,272 TPS** |
| 架构 §5 万在线需求 | 5 万在线，玩家持久化每 30s 批量落库 | 远低于 78k |
| 余量 | 78k / 需求 | 极充裕 |

> 单分库 9784 TPS 已预留 ~2× 余量（验收仅 5000）。8 分库并行理论 ~78k TPS，对 5 万在线的玩家持久化（30s 批量、断线即时）完全够用。

## 4. ⚠️ 实际并行数尚未实测（待执行）

本沙箱 Docker 守护不可达、无 sysbench，故 **8 路并行实测未跑**。需满足：
- 带 Docker + sysbench 的主机（或 `build-modules-on` CI 挂 sysbench 服务容器）。
- 8 分库 Docker compose 栈已起（`docker/docker-compose.yml`）。

### 执行命令
```bash
# 宿主机直连各分片主库端口 (默认 33060..33067, 按 docker-compose 端口映射调整)
bash benchmark/sysbench/run-8shard.sh
# 结果: benchmark/sysbench/results/cluster_summary.txt (8 路 TPS 之和)
```

### 宿主机 NAT 隔离时的容器内备选
`run-8shard.sh` 末尾已附 `IN_CONTAINER` 段：每分片容器内 `sysbench ... --mysql-socket=...` 直跑，避开采编排假象。

## 5. 预期与验收

- **预期**：8 路并行 TPS ≈ 70k~78k（线性度取决于宿主机 IO / 容器资源隔离；若共享同一物理盘可能略低于线性）。
- **验收**：集群写入容量 ≥ 40k TPS（8×5000 架构基准）即达标；实测填入 `cluster_summary.txt` 后闭环。
- 若实测显著低于线性：检查是否为共享存储 / 连接池瓶颈 / 代理层（ShardingSphere）开销，针对性调优后再测。

## 6. 复现清单

| 项 | 命令 / 文件 |
|----|------|
| 单分库基线 | `docs/database/shard-tps-bench.md`（容器内直插法） |
| 8 路并行 | `benchmark/sysbench/run-8shard.sh` |
| 分库栈 | `docker/docker-compose.yml`（8 主 8 从 + SS） |
| 实测回填 | `benchmark/sysbench/results/cluster_summary.txt` |
