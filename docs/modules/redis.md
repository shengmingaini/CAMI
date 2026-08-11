# 在线态存储模块（Redis）详细设计

> **文档状态**: [PROTOTYPE]  
> **版本**: v1.0.0  
> **更新日期**: 2026-08-27 (Week3 周四 — Redis 在线态)  
> **所属层**: 接入层（gateway）  
> **上游规约**: `docs/architecture/architecture-spec.md`  
> **关联 ADR**: ADR-002（模块边界）

---

## 1. 模块概述

- **定位**：网关接入层的玩家在线态中枢——维护 `player_id -> 所在 game 后端` 的映射，支撑路由（Day3）、连接迁移（Day5）与鉴权（Day1）的"玩家此刻在哪"查询。
- **核心职责**：
  1. **在线态读写**：`set_online / set_offline / get_backend / is_online / prune_expired`。
  2. **16 分片路由**：`player_id` → 16 个 Redis 分片之一（一致性哈希，分片均衡）。
  3. **故障切换**：分片下线时路由层重路由（自动切换的路由体现；集群级 failover 由 Redis Cluster 原生提供）。
- **不在职责内（边界）**：鉴权/加密（Day1）、限流（Day2）、后端选择（Day3 router）、迁移编排（Day5）。本模块只管"在线态的存储与分片定位"。

## 2. 架构约束与边界

- 是否拥有 `Player` 对象：否。仅以 `player_id` 为键，存 `backend` 字符串。
- **重型依赖门控**：真实后端 = Redis Cluster（redis-plus-plus），门控 `CAMI_BUILD_MODULES=ON`（vcpkg 声明）。`CAMI_BUILD_MODULES=OFF`（轻量 CI）下真实后端不编译，内存后端 `InMemoryState` + `ShardRouter` 始终可用，selfcheck / 单测确定性验证分片均衡与故障切换。
- `ShardRouter`：纯算法（FNV-1a + fmix64 + 黄金比例探测），零外部依赖，单测/selfcheck 直接验证。

## 3. 对外接口（C++ 签名级）

```cpp
class OnlineStateStore {  // 抽象基类
    void set_online(uint64_t player_id, const std::string& backend);
    void set_offline(uint64_t player_id);
    std::optional<std::string> get_backend(uint64_t player_id) const;
    bool is_online(uint64_t player_id) const;
    void prune_expired(int64_t now_ms);
    std::size_t size() const;
};

class ShardRouter {  // 16 分片一致性哈希 + 故障重路由
    int shard_of(uint64_t player_id) const;   // [0,16)；-1=全下线
    void set_shard_down(int shard, bool down);
    bool is_shard_down(int shard) const;
};
```

## 4. 核心数据结构

- `OnlineEntry { backend, expiry_ms }`：内存条目（expiry_ms=0 永不过期）。
- `ShardRouter::down_`：`vector<char>` 标记各分片下线状态。

## 5. 协议引用

- `get_backend(player_id)` 在路由（Day3）/ 连接迁移（Day5）需要定位玩家所在后端时调用。
- 真实后端键设计：`online:<player_id>` → HASH{backend}；过期由 Redis 侧 TTL / 键空间通知承担。

## 6. 性能预算（验收）

| 指标 | 红线 | 本模块保障 |
|------|------|-----------|
| 分片均衡 | max < 2×avg | 16 分片实测均匀（selfcheck + 单测实证，20000 key） |
| 故障切换 | 下线分片 key 全重路由、分布仍均衡 | ShardRouter 黄金比例探测重路由（selfcheck 实证） |
| 单次读写延迟 | 亚毫秒（Redis 内存） | redis-plus-plus 直连；OFF 内存后端 O(1) |

> 真实 Redis Cluster 的端到端延迟 / failover 秒级指标需在 `MODULES=ON` + 运行 Redis Cluster 的 CI job 验证（沙箱 OFF 不覆盖）。

## 7. 并发模型

- `InMemoryState`：内部 `unordered_map` 未加锁（原型单线程路径）；多线程共享需外部加锁或换 Redis 后端。
- `ShardRouter`：无状态、线程安全（只读 `down_` 在未并发改线下状态时）。
- `RedisClusterState`：redis-plus-plus 客户端线程安全（连接池），天然支持并发。

## 8. 依赖方向

```
[路由/迁移/鉴权] ──→ [redis] ──(MODULES=ON)──→ [Redis Cluster (16 分片)]
                  │  OFF: InMemoryState + ShardRouter（零依赖）
```

## 9. 关联 ADR / 架构章节

| 决策 | 编号 |
|------|------|
| 模块边界 WoW 模式 | ADR-002 |
| 重型依赖默认关闭 | 项目构建约定（`CAMI_BUILD_MODULES`） |
| 后期模块不 mutate 早期模块 | 工作流纪律（集成缝仅文档化） |

## 10. 开放问题 / 后续

- **真实后端验证**：`RedisClusterState` 仅 `MODULES=ON` 编译，沙箱 OFF 未端到端验证；需 vcpkg + 运行 Redis Cluster 的 CI job 闭环。
- **在线态与迁移联动**：Day5 连接迁移设计将调用 `set_online/set_offline` 维护迁移期间玩家落点。
- **分片数配置化**：当前 16 为常量（kShards），生产应来自集群拓扑配置。
- **安全红线**：Redis 连接串/密码来自配置中心，禁止硬编码。
