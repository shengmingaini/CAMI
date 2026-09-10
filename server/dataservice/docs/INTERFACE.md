# DataService Interface — 接口契约（TASK-026）

> 统一持久化访问层。GameNode 禁止直连外部存储；一切读写经本层接口，
> 具体实现后续接入（本任务仅提供内存实现，无具体数据库依赖）。

## 1. 三套接口

| 接口 | 角色 | 关键方法 |
|---|---|---|
| `IDataStore` | 权威持久化（对应外部关系型/对象存储） | `Load` / `Save` / `Delete` / `BatchLoad` / `BatchSave` |
| `ICache` | 易失缓存（对应外部键值缓存） | `Get` / `Put` / `Invalidate` / `InvalidatePrefix` |
| `IRepository<T>` | 领域对象泛型仓储（建立在 IDataStore 之上） | `GetById` / `Put` / `Remove` |

`IRepository<T>` 为模板接口，T 由具体业务仓储负责 `T <-> Record` 编解码。

## 2. 数据模型

```cpp
using DataKey = std::string;            // "<domain>:<id>"，如 "character:10086"
struct Record { DataKey key; std::string payload; uint32_t version{0}; core::SteadyTime updated_at; };
struct VersionCheck { uint32_t expected_version{0}; bool required{true}; };  // 乐观锁
```

## 3. 读/写路径

- **cache-aside 读**：`Cache.Get` → 命中返回；未命中 → `Store.Load` → `Cache.Put` → 返回。
- **write-behind 写**：写 `Cache` → 标记脏 → 异步批量 `Flush` 到 `Store`（本任务内存实现，Flush 同步落盘）。

## 4. 版本冲突（乐观锁）

`Save`/`Delete` 带 `VersionCheck{required=true}` 时，期望版本必须与实际版本一致；
不符返回 `core::ErrorCode::VERSION_CONFLICT`（`domain::kData`），**禁止静默覆盖**。
调用方决定重试或放弃。

## 5. 批量逐条结果

`BatchSave` 返回 `std::vector<BatchOutcome>`（每键一条 `{key, ok, code}`），
部分失败**逐条透传**，不得整批吞错；成功条目照常生效。

## 6. 指标（DataServiceStats）

`cache_hit_rate` / `pending_writes`（脏队列长度）/ `flush_latency_us` /
`conflict_count` / `flush_count`。背压：脏队列达 `max_pending` 时 `Save` 返回 `BUSY`。

## 7. 内存实现（交付物）

- `InMemoryStore`：`IDataStore` 的内存实现（O(1) 哈希 + 版本校验）。
- `InMemoryCache`：`ICache` 的内存实现（有界 LRU + TTL 过期）。
- `DataService`：组合层（cache-aside + write-behind + 指标 + 背压）。
- `FakeDataStore`：可注入延迟/错误/版本冲突的测试替身（验证重试与冲突路径）。

## 8. 红线

- 接口只管键值与版本，禁止混入业务语义。
- 禁止 GameNode 在 Tick 内同步等待 DataService。
- 禁止把实时状态（HP/位置）写入本层。
- 本任务不接入任何具体外部存储（仅接口 + 内存实现）。
