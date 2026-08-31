# Gateway Router 模块说明（TASK-010）

> 本目录文档对应 TASK-010 · Gateway Router（Phase 2 · Gateway）。
> 同属 `server/gateway` 的 Session 管理文档见 `../`（TASK-009）。

## 1. 职责

Gateway 路由层负责把**上行包**路由到正确的 GameNode / Scene Owner：

- **PlayerRouter** —— `PlayerId → GameNodeId`（缓存 miss 走注册中心一致性哈希选节点并回填）。
- **SceneRouter** —— `SceneId → GameNodeId`（Scene 的 Owner 必须唯一；重复 Bind 到不同节点返回 `VERSION_CONFLICT`）。
- **NodeRegistry** —— 节点注册、心跳、健康三态机（Healthy→Suspect→Dead）、一致性哈希 `Pick`。
- **RouteCache** —— 16 分片有界 LRU，节点失效时按 value 批量失效。
- **GatewayRouter** —— 门面，装配上述四者成一条可测转发路径，并订阅 `NodeDead` 事件批量失效缓存。

## 2. 全链路

```
客户端 ──Login──▶ Gateway ──鉴权(TASK-009)──▶ PlayerRouter.Route(player)
                                                    │ cache miss
                                                    ▼
                                              NodeRegistry.Pick(GameNode, player)
                                                    │ 一致性哈希（稳定 + 近均匀）
                                                    ▼
                                              选 GameNode ──回填 RouteCache
                                                    │
                              EnterScene(scene) ─────┘
                                                    ▼
                              SceneRouter.OwnerOf(scene) / Bind（首占者分配）
                                                    ▼
                                          GameNode 收到转发包
```

集成测试断言 `Session.game_node_id` / `Session.scene_id` 被正确填充（见 TEST.md / §17）。

## 3. 关键不变量

1. **Scene Owner 唯一性**：同一 Scene 只有一个权威 Owner，重复 Bind（不同节点）→ `VERSION_CONFLICT`；同节点重复 Bind → 幂等 OK。
2. **路由表有界**：RouteCache 默认 100k 条目，LRU 淘汰，禁止无界增长。
3. **无全局锁**：RouteCache 16 分片每片独立 `std::mutex`；NodeRegistry 由 Gateway NetworkThread 单写者独占，无锁。
4. **失败可见**：目标节点全不可达返回 `BUSY`（明确错误 + 指标 + 日志），禁止静默丢包。
5. **节点失效可恢复**：GameNode 心跳停止 3 次（15s @5s 间隔）→ 判 Dead → 发布 `NodeDead` → 缓存批量失效 → 后续请求不再打向死节点。

## 4. 模块边界

- 归属目录：`server/gateway/{include,src,tests}/route/`，公开头只在 `include/mmo/gateway/route/`。
- 上游依赖：`engine/core`（error / time / bus，TASK-007）、`server/gateway/session`（TASK-009）。禁止 `#include` 其 `src/`。
- 下游禁止 `#include` 本模块 `src/`。

## 5. 文档索引

| 文档 | 内容 |
|---|---|
| [INTERFACE.md](./INTERFACE.md) | 公开接口参考（类、方法、签名、语义） |
| [DEPENDENCY.md](./DEPENDENCY.md) | 依赖与依赖者 |
| [PERFORMANCE.md](./PERFORMANCE.md) | 实测性能与阈值 |
| [TEST.md](./TEST.md) | 测试清单与运行方式 |
