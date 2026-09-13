# Operations — CAMI MMORPG

> 本文件为 TASK-038 交付物之一（验收脚本 `require_files` 检查项）。
> 与 `DEPLOYMENT.md`（怎么部署）互补：本文件聚焦「部署之后怎么运维」。

## 1. 健康检查

| 组件 | 端口 | 探测方式 |
|------|------|----------|
| Gateway | 9001 | TCP `:9001` / `GET /healthz` |
| GameNode | 9101 | TCP `:9101` |
| ControlService | 9100 | TCP `:9100` |
| Redis | 6379 | `redis-cli ping` → PONG |
| MySQL | 3306 | `mysqladmin ping` → alive |

k8s 清单已内置 `readinessProbe` / `livenessProbe`（见 `deploy/k8s/`）。

## 2. 日常操作

```bash
# 启动 / 停止（本地）
docker compose -f deploy/docker/docker-compose.yml up -d
docker compose -f deploy/docker/docker-compose.yml down

# 扩缩容（k8s）
kubectl scale deploy/gamenode --replicas=80
kubectl rollout status deploy/gateway

# 热更（Lua，仅 Tick Safe Point 切换，禁止运行中热切）
#   由 ControlService 触发；流程见 docs/architecture/ 与 TASK-012/013。
```

## 3. 压测与容量（Capacity）

```bash
# CCU 阶梯：100/500/1000 本地 sim 直跑；5000+ 需 --gateway 真实集群
bash tools/load/run_ladder.sh                 # 本地 100/500/1000
bash tools/load/run_ladder.sh --full --gateway 10.0.0.5:9001

# 单点压测（生成 bench/load_<N>.txt）
.verity_041/Release/bin/bot_bench --bots 1000 --duration 300 --sim
```

- 阈值断言：`tick_p99_ms <= 8` 且 `error_rate <= 0.001`（详见 `scripts/verify/task-038.sh`）。
- 历史数据归档于 `bench/load_*.txt`；容量结论见 `docs/architecture/capacity-report.md`（**如实记录，未实测不宣称**）。

## 4. 混沌演练（Chaos）

七项故障注入 + 恢复断言，逐项独立、可单独重跑：

```bash
bash tools/chaos/run_chaos.sh --dry                       # 全部 dry-run
bash tools/chaos/run_chaos.sh --yes --service gamenode    # compose 靶
# 单项示例（默认 dry-run，--yes 才真正注入）：
bash tools/chaos/kill_gamenode.sh --pid 12345 --yes
bash tools/chaos/gateway_crash.sh --service gateway --yes
bash tools/chaos/redis_failure.sh  --service redis   --yes
bash tools/chaos/mysql_failure.sh  --service mysql   --yes
bash tools/chaos/net_partition.sh  --service gamenode --yes
bash tools/chaos/cpu_saturation.sh --service gamenode --yes
bash tools/chaos/memory_pressure.sh --service gamenode --yes
```

| 项目 | 验证目标 |
|------|----------|
| GameNode Crash | 心跳丢失 → Dead → 故障接管 → 玩家重连 ≤15s |
| Gateway Crash | 多副本无单点，新连接被健康网关节点接管 |
| Redis Failure | 热路径不依赖同步 Redis；恢复后状态重建 |
| MySQL Failure | GameNode 不直连 MySQL；异步持久化补刷 |
| Network Partition | 分区解除后集群自愈合 |
| CPU / Memory Pressure | 压力下 Tick 不雪崩、OOM 前优雅降级 |

## 5. 弱网模拟（Netsim）

六种弱网场景，注入 → 探针 → 拆除：

```bash
bash tools/netsim/run_netsim.sh --dry                  # 仅打印 tc 命令
bash tools/netsim/run_netsim.sh --yes --iface eth0     # 主机侧
bash tools/netsim/run_netsim.sh --yes --service gamenode
```

场景：高延迟 / 丢包 / 带宽限制 / 抖动 / 随机 RST / DNS 抖动。

## 6. 故障排查清单

- **Tick 超时告警**：先查 GameNode CPU/内存压力（`tools/chaos/cpu_saturation.sh` 可复现）。
- **玩家重连失败**：确认 Gateway 健康检查与 ControlService 接管日志；确认 Session 在 Redis 未丢失。
- **持久化丢失**：确认 DataService 异步队列已刷盘，Redis RDB **不是**唯一恢复源。
- **配置漂移**：以 `deploy/k8s/01-configmap.yaml` 为唯一来源，禁止节点本地硬编码分片数。

## 7. 升级与回滚

- 二进制升级走镜像 tag；k8s `kubectl rollout undo deploy/<svc>` 回滚。
- Lua 热更带 `ScriptVersion` / `ConfigVersion`，失败自动 Rollback（见 TASK-012/013）。
- 任何架构级变更须经 RFC / Architecture Change Request 并人工批准（见项目总规范）。
