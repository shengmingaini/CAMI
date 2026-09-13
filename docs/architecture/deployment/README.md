# 部署拓扑（TASK-038 交付 · docs/architecture/deployment/）

## 1. 目标拓扑（§24 / §34）

```
                         [ Clients / Bots ]
                                │
                          Protocol (Envelope)
                                │
                          ┌─────┴─────┐
                       [ Gateway ×10 active + failover ]
                          │   gRPC    │
              ┌───────────┼───────────┼────────────┐
           [ GameNode ] [ GameNode ] [ GameNode ] ×50 active + failover
              │  Scene/AOI/Movement/Combat/... (同进程模块)
              └───────────────┬───────────────┘
                          DataService
                       ┌────┴────┐
                   [ Redis ]   [ MySQL 8 shards ]
              [ ControlService ] (节点管理/配置/健康/运维)
```

## 2. 部署单元

| 单元 | 镜像/包 | 副本 | 说明 |
|---|---|---|---|
| Gateway | `deploy/docker/Dockerfile.gateway` | 10 + failover | 连接/路由/限流/心跳 |
| GameNode | `deploy/docker/Dockerfile.gamenode` | 50 + failover | 实时游戏逻辑 |
| DataService | 复用 redis/mysql 官方镜像 | 按分片 | 最终持久化 |
| ControlService | `deploy/docker/Dockerfile.control` | 1 + failover | 运维控制面 |

## 3. 本地一键冒烟（docker compose）

```bash
cd deploy/docker
docker compose up -d            # 起 Gateway + GameNode + Redis + MySQL
bin/bot_bench --bots 100 --duration 60 --gateway 127.0.0.1:9001
```

## 4. Kubernetes（deploy/k8s）

- `namespace.yaml`：隔离命名空间 `mmorpg`。
- `gateway-deployment.yaml` + `gateway-service.yaml`：Gateway 无状态，HPA 按 CCU。
- `gamenode-statefulset.yaml`：GameNode（有状态，Scene 归属）。
- `redis.yaml` / `mysql.yaml`：数据层（生产用托管实例或 StatefulSet + PV）。
- `control-deployment.yaml`：ControlService。
- 水平扩容：调 `gamenode-statefulset` 副本数并重新跑 `tools/load/run_ladder.sh` 验证。

## 5. 故障恢复（对应 TASK-037 / §38.4）

- 任意单元崩溃由 ControlService 检测并触发替换/重连；玩家可重连优先。
- 禁止把 Redis RDB 快照作为唯一状态恢复方案（§17/§33）。
- Chaos 演练脚本见 `tools/chaos/`。
