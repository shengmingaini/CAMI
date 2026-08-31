# Gateway Router · 测试清单（TASK-010）

> 测试入口：`server/gateway/tests/route_test.cpp`，ctest 名 `Gateway_Route.Suite`。
> 输出走 `test_print.h`（禁止裸 `cout`/`printf`）。13 个测试函数，60+ 断言。

## 1. 单元测试（§16）

| 函数 | 覆盖 |
|---|---|
| `TestNodeRegistry` | Register（重复幂等）、Heartbeat 刷新 load、未注册心跳/注销的 NOT_FOUND |
| `TestHealthStateMachine` | Healthy→Suspect（1 次丢心跳）→Dead（3 次丢心跳被剔除） |
| `TestPickAllDeadReturnsBusy` | 全节点不可用时 `Pick` 返回 `BUSY` |
| `TestConsistentHashDistribution` | 1 万玩家落到 10 节点，最大偏差 < 15%（实测 ~5.8%）；同 key 稳定命中 |
| `TestRouteCache` | Put/Get、LRU 容量钳制、Evictions>0、EraseByValue 缩容 |
| `TestRouteCacheHitRate` | 预热后命中率 > 90%（冷查询不塌方） |
| `TestPlayerRouter` | cache miss 分配并回填、再次查询命中、Invalidate 后重路由 |
| `TestSceneRouter` | 未绑定 NOT_FOUND、Bind、重复 Bind 异节点 VERSION_CONFLICT、同节点幂等、非 Owner Unbind VERSION_CONFLICT、Owner Unbind OK |

## 2. 集成测试（§17）

| 函数 | 覆盖 |
|---|---|
| `TestFullLink` | 1 Gateway + 2 GameNode 替身：Login→选节点→EnterScene（按 PlayerID 稳定路由）→未知场景按 Scene 绑定→全未知 NOT_FOUND。断言 `Session.game_node_id` / `scene_id` 被正确填充 |

## 3. 失败测试（§19）

| 函数 | 覆盖 |
|---|---|
| `TestNodeDeadCacheEviction` | 节点心跳停止→Tick 判 Dead→`NodeDead` 经 `EventBus::Drain` 派发→`OnNodeDead` 批量失效缓存→后续请求不再打向死节点 |
| `TestAllNodesDown` | 全节点不可用时 `RouteUpstream` 返回 `BUSY` 而非崩溃 |
| `TestSceneOwnerConflictFailure` | 两个节点同时 Bind 同一 Scene，第二个返回 `VERSION_CONFLICT` |
| `TestCacheFullNoCrash` | 缓存打满持续淘汰，命中率仍 > 90%，不崩溃 |

## 4. 关键陷阱（测试中已踩并修正）

1. **EventBus 异步派发**：`NodeRegistry::Tick` 的 `NodeDead` 是 `Publish`（入队），必须宿主线程 `bus.Drain()` 才会触发 `OnNodeDead` 缓存失效。测试若只 `Tick` 不 `Drain`，缓存不会失效。
2. **一致性哈希必须用整数累积**：Jump Consistent Hash 的 `b`/`j` 必须为 `int64`；若用 `double` 累积会令桶边界漂移，导致分布系统性偏斜（实测 bucket 1 偏高 ~19%）。
3. **累计命中率口径**：`RouteCache::HitRate()` 为全生命周期累计值；benchmark 须在 warmup 后 `ResetStats()` 再测稳态命中率，否则 warmup 的首次 miss 把命中率拖到 ~0.92。

## 5. 运行

```bash
# ctest 仅跑路由套件
ctest -R Gateway_Route --output-on-failure

# 或直跑可执行
./build/Release/bin/gateway_route_test.exe
```
