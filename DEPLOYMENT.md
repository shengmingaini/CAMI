# Deployment — CAMI MMORPG

> 本文件为 TASK-038 交付物之一（验收脚本 `require_files` 检查项）。
> 架构总规范见 `docs/architecture/architecture.md`；容量与真实测量结果见 `docs/architecture/capacity-report.md`。

## 1. 进程拓扑（4 类核心进程 + 2 个数据依赖）

```
                  Client
                     │  Protocol (Envelope: Command/Query/Event/Response/Heartbeat)
                     ▼
                  Gateway            :9001  连接 / Session / 心跳 / 安全 / 限流 / 路由
                     │  gRPC
        ┌────────────┼─────────────────────────┐
        ▼            ▼                         ▼
     GameNode     GameNode                GameNode     :9101  实时逻辑（20Hz 固定 Tick）
        │  DataService（统一封装 Redis + MySQL，GameNode 禁止直连 DB）
        ▼
   ┌────────┐   ┌────────┐
   │ Redis  │   │ MySQL  │     Session/Cache/Routing/Ranking · 最终持久化
   └────────┘   └────────┘
        ▲
   ControlService  :9100  Config / 节点管理 / 版本 / 热更 / 部署
```

- **Gateway**：初始 10 active + failover 余量（横向扩展无状态）。
- **GameNode**：初始 50 active + failover 余量；在线玩家实时状态由当前 Scene/Instance 持有（单一权威 Owner，见 `docs/architecture/state-ownership.md`）。
- **DataService**：业务代码只依赖其统一接口，不感知 Redis/MySQL 实现。
- **ControlService**：控制面，2 副本冗余。

## 2. 本地开发 / 冒烟（Docker Compose）

```bash
# 前置：已构建 build/Release/bin/{gateway,gamenode,control}
docker compose -f deploy/docker/docker-compose.yml up --build
# 冒烟：100 CCU
.verity_041/Release/bin/bot_bench --bots 100 --duration 30 --gateway 127.0.0.1:9001
```

镜像构建见 `deploy/docker/Dockerfile.{gateway,gamenode,control}`（多阶段骨架，真实二进制由
`cmake --build --target <svc>` 产出后 COPY 进最终镜像）。

## 3. Kubernetes

清单位于 `deploy/k8s/`，按编号顺序应用：

```bash
kubectl apply -f deploy/k8s/00-namespace.yaml
kubectl apply -f deploy/k8s/01-configmap.yaml
kubectl apply -f deploy/k8s/02-redis.yaml
kubectl apply -f deploy/k8s/03-mysql.yaml
kubectl apply -f deploy/k8s/04-gateway.yaml
kubectl apply -f deploy/k8s/05-gamenode.yaml
kubectl apply -f deploy/k8s/06-control.yaml
```

- 镜像：`mmorpg/{gateway,gamenode,control}:latest`（由 CI 基于上述 Dockerfile 产出）。
- 配置：来自 `01-configmap.yaml`（端口、分片数 `SHARD_COUNT=8`、Tick 频率等）。
- 数据层：`redis` / `mysql` StatefulSet + 独立 Service；MySQL 需要 `mmorpg-secrets` 提供 `mysql-root-password`。
- 扩缩容：`kubectl scale deploy/gamenode --replicas=80`。

## 4. 配置项（关键）

| 变量 | 默认 | 说明 |
|------|------|------|
| `GATEWAY_LISTEN` | `0.0.0.0:9001` | 客户端接入端口 |
| `GAMENODE_GRPC` | `0.0.0.0:9101` | GameNode gRPC 端口 |
| `CONTROL_LISTEN` | `0.0.0.0:9100` | ControlService 端口 |
| `REDIS_ADDR` / `MYSQL_ADDR` | `redis:6379` / `mysql:3306` | 数据层地址 |
| `TICK_HZ` | `20` | GameNode 固定 Tick（50ms） |
| `SHARD_COUNT` | `8` | 逻辑分片数（业务层不得写死） |
| `HOTRELOAD_SAFE_POINT` | `true` | Lua 仅 Tick Safe Point 切换 |

## 5. 故障恢复（概览）

- GameNode 故障：健康检测（3 次心跳丢失 → Dead）→ 选同角色低负载替换节点 → 玩家重连恢复（目标 ≤15s）。详见 `docs/architecture/sequence/README.md` 与 TASK-037。
- 禁止把 Redis RDB 快照作为唯一状态恢复机制（见 §17）。
- 端到端演练脚本见 `tools/chaos/run_chaos.sh`（七项故障 + 恢复断言）。

## 6. 容量与扩展路径

- 第一基准：1000 玩家/Scene，Tick P95 < 5ms、P99 < 8ms（实测见 `bench/` 与 `capacity-report.md`）。
- 扩展目标：100 → 500 → 1k → 5k → 10k → 20k → 50k CCU；具体各档真实数据须由 `tools/load/run_ladder.sh` 实测，未实测不得宣称达成。
