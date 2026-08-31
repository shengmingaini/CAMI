# Gateway Router · 性能实测（TASK-010）

> 环境：Windows + MSYS2 MinGW g++ 16.1.0，Release，Ninja，vcpkg manifest（baseline aae277ac）。
> 数据来自 `route_bench`（100 万次查询）与 `gateway_route_test`，机器可读指标写入 `bench/gateway_route.txt`。

## 1. 实测指标（Release，1M lookups / 10 节点 / warm=90k）

| 指标 | 实测 | 阈值（§22） | 结论 |
|---|---|---|---|
| `route_lookup_ns` | **31.0 ns** | ≤ 100 ns | ✅ |
| `cache_hit_rate` | **1.000** | ≥ 0.99 | ✅ |
| `registry_tick_us_1k_nodes` | **1.7 µs** | ≤ 100 µs | ✅ |
| `cache_evict_ns` | ~0 ns* | — | 单次淘汰远低于 1µs |
| 一致性哈希分布偏差（10 节点 / 1万玩家） | **~5.8%** | < 15% | ✅ |

> \* `cache_evict_ns` 为单点 `MonotonicClock::Now()` 差值，时钟分辨率内未推进则记 0，属测量产物；批量失效路径由 `EraseByValue` 跨 16 分片锁内删除，开销可忽略。

## 2. 设计要点（为什么达标）

- **零拷贝热路径**：`RouteUpstream` 命中即 `RouteCache::Get`（`std::lock_guard` + `unordered_map::find` + `list::splice`），无堆分配、无 IO。
- **无全局锁**：RouteCache 16 分片，每片独立 `std::mutex`；16 路并发争用被均摊，规避单锁瓶颈（§21 红线）。
- **一致性哈希 O(log n)**：Jump Consistent Hash，节点增删仅 ~1/n 键重映射，避免全量 rehash。
- **单写者注册中心**：NodeRegistry 由 Gateway NetworkThread 独占驱动，无锁；`Tick` 健康扫描 O(n) 且不在转发路径。

## 3. 缓存容量与命中率

- 默认容量 100k 条目，按 16 分片 → 每片 6250。
- benchmark 预热 90k 玩家（< 容量，无淘汰），重置统计后测稳态：命中率 100%。
- 容量打满场景（见 `TestCacheFullNoCrash` / `TestRouteCacheHitRate`）：持续 LRU 淘汰下命中率仍 > 90%，不塌方。

## 4. 复测命令

```bash
# Debug / Release 双构建 + 测试 + benchmark + 阈值断言
bash scripts/verify/task-010.sh

# 单独跑 benchmark
./build/Release/bin/route_bench.exe --lookups 1000000
cat bench/gateway_route.txt
```
