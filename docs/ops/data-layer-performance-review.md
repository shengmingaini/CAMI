# 数据层性能审查报告（2026-08-12）

> **范围**: data/ 全模块（redis_proxy / sync / version / mysql_proxy / data_service / kafka_flush）
> **方法**: 缓存读/写热路径逐行走查 + 批量落库链路（30s 周期）+ 5 万在线架构对齐
> **结论**: 架构与一致性设计正确（CAS/版本源/读穿写回均合理），但**缓存读热路径 3 把锁**、
> **批量落库 5 万 key 即 5 万 RTT + 5 万次 SQL prepare**——这是生产环境下吞吐与延迟的最大瓶颈。

---

## 1. 热路径瓶颈（P0）

### 1.1 缓存读命中 = 3 把锁 + LRU 链表搬移 ⚠️

**现状**（`cache_proxy.cpp` Get / `cache_proxy.h` InMemoryBackend）：
- 每次 `Get` 命中：① `backend` 内部 `mu_` 锁 + **读也搬 LRU 链表**（erase+push_front，2 次 list 操作）；② `stats_mu_` 锁（hits_++）；③ `HotKeyDetector::record` 锁 + string 拷贝（enable_hot 默认 true，**每读必 record**）。
- 5 万玩家 × 高频属性/背包读 → **每读 3 把锁竞争**。GameNode 多线程并发访问缓存代理时，锁竞争直接拖垮吞吐（红线：无全局锁）。

**优化方案**：
1. **统计计数改 `std::atomic<uint64_t>`**（hits_/misses_）——热路径省 stats_mu_ 锁。
2. **热点识别采样**：`record` 每 N 次调用采样 1 次（N=64）——热 key 每秒数百次访问，采样后仍远超阈值；锁频率降 64×。
3. **读命中不搬 LRU**：LRU 链表仅写侧维护（写回 Put 频繁，热 key 写侧已刷新位置）——省读路径 2 次 list 操作。近似 LRU 驱逐对 MMO 缓存（玩家数据写频繁）足够。

**收益**：读热路径 3 锁 → 1 锁（backend 内部），list 操作 ×2 消除，热点锁频率 ↓64×。

### 1.2 批量落库逐 key 取数 = 5 万 RTT ⚠️

**现状**（`cache_proxy.cpp` FlushDirty）：对 dirty 集合**逐 key `backend_.Get`** 取缓存值再发布——Redis 后端下 = 5 万次网络 RTT（30s 周期内一次 5 万 RTT 的突发）。

**优化方案**：
1. **dirty 集合存 {key, value}**（WriteBack 缓冲，`unordered_map<string,string>`）：Put 时 value 直接入缓冲，FlushDirty 整体 swap 批量出队——**免 flush 时回读缓存**（5 万 RTT → 0），同时根治"dirty key 被驱逐导致 value 丢失告警"的问题。
2. 若仍走缓存取数（不存值方案）：用 **Redis MGet pipeline**（`RedisBackend::MGet` 已实现）一次取全部——5 万 RTT → 1 次。

**收益**：FlushDirty 从"5 万次串行 RTT"→"1 次批量交换"（dirty 存值）或"1 次 MGet pipeline"。

### 1.3 MySQL 落库每 key 一次 prepare ⚠️

**现状**（`mysql_backing_store.cpp`）：Store/CasStore 每条语句都 `mysql_stmt_init + prepare + bind + execute + close`——**prepare 是网络往返 + 服务端解析**，5 万 key = 5 万次 prepare。

**优化方案**：
1. **statement 预编译缓存**：同构 SQL 的 `MYSQL_STMT*` 缓存复用（prepare 一次，多次 execute），`mysql_stmt_close` 在析构统一释放。
2. 进阶：批量 multi-row UPSERT（`INSERT ... ON DUPLICATE KEY UPDATE` 一次多条）——player_state 版本语义下需评估，先做 1。

**收益**：5 万次 prepare → 1 次；落库吞吐提升一个量级。

### 1.4 Kafka 无压缩逐条 produce

**现状**（`kafka_flush.cpp`）：PublishBatch 逐条 `produce`（无 `compression.type`，默认无压缩）；尾部 `flush()` 阻塞确认（可接受，30s 周期）。

**优化方案**：producer 配置加 `compression.type=lz4`（或 zstd）+ `linger.ms=10` + `batch.num.messages` 调优——玩家 payload 压缩比高，省带宽、提升批吞吐。`flush()` 保持（确认语义正确）。

**收益**：Kafka 带宽 ↓ 60-80%（lz4 对 protobuf 序列化数据），吞吐 ↑。

## 2. 容量/配置问题（P1）

| 项 | 现状 | 建议 |
|----|------|------|
| dirty 集合上限 | 无上限（5 万 key 常驻可接受，但无背压） | 加容量上限 + 超限强制 Flush 背压 |
| InMemoryBackend 容量 | 构造参数 10 万 | 接入配置中心（Scale-to-Fit） |
| SyncManager 周期 | 30s 固定（构造可传） | 配置化（起步 30s，可调） |
| gRPC BatchPut | 已实现 | 确认 DataClient 批量路径利用率 |

## 3. 设置合理性确认（无需改动）

| 项 | 结论 |
|----|------|
| 版本 CAS（DB 权威版本源，方案 B） | ✅ 正确，防多副本并发覆盖 |
| 读穿回源 + 回填、写回 + 30s 批量、断线立即持久化 | ✅ 正确 |
| Kafka at-least-once + DLQ + enable.auto.commit=false | ✅ 正确 |
| 异步落库 sink + 同步回退（OFF） | ✅ 正确 |
| RedisBackend pipeline（MGet/MPut） | ✅ 已具备，FlushDirty 未用（见 1.2） |

## 4. 收益预估（5 万在线基准）

| 指标 | 现状 | 优化后 | 提升 |
|------|------|--------|------|
| 读命中热路径 | 3 锁 + 2 list 操作 + 热点锁 | 1 锁（backend）+ 0 list + 采样 | **锁竞争 ↓ 70%+** |
| FlushDirty 取数 | 5 万 RTT | 0 RTT（dirty 存值） | **×万级** |
| MySQL 落库 | 5 万 prepare | 1 prepare + 复用 | **一个量级** |
| Kafka 带宽 | 未压缩 | lz4 | **↓ 60-80%** |
| 脏值丢失告警 | 缓存驱逐即丢 | 缓冲持有，不丢 | 根治 |

## 5. 实施建议（按 ROI）

| # | 任务 | 优先级 | 说明 |
|---|------|--------|------|
| 1 | CacheProxy stats 改 atomic + 热点采样 | P0 | 热路径去 2 锁 |
| 2 | InMemoryBackend 读命中不搬 LRU | P0 | 省 list 操作 |
| 3 | dirty 集合存 {key,value}，FlushDirty 免回读 | P0 | 根治 RTT 突发 + 脏值丢失 |
| 4 | MySQL statement 预编译缓存复用 | P0 | prepare 5 万→1 |
| 5 | Kafka lz4 压缩 + linger 批调优 | P1 | 配置级改动 |
| 6 | dirty 容量上限背压 | P1 | 防无限累积 |

> 注：#1-4 均为 data 层内部改动，不改变 BackingStore/CacheProxy 对外语义；cache_proxy_demo/单测同步更新。
> 与网关层审查（docs/ops/gateway-performance-review.md）正交，可并行推进。

## 落地状态（2026-08-12 已实现 ✅）

| # | 任务 | 状态 |
|---|------|------|
| 1 | CacheProxy stats 改 atomic + 热点采样 | ✅ `hits_/misses_` 原子无锁；`HotKeyDetector` 加 sample_ratio（构造可配，生产建议 64） |
| 2 | InMemoryBackend 读命中不搬 LRU | ✅ Get 仅查找，写侧维护 LRU 位置 |
| 3 | dirty 集合存 {key,value}，FlushDirty 免回读 | ✅ `dirty_` 改 map 存值；FlushDirty 整体 swap + 免 backend.Get；失败重入队不覆盖新值；根治驱逐丢值告警 |
| 4 | MySQL statement 预编译缓存复用 | ✅ 5 个 stmt 缓存（upsert/delete/cas_upd/cas_sel/cas_ins），prepare 一次 execute 复用；断线重连自动清缓存 |
| 5 | Kafka lz4 压缩 + linger 批调优 | ⏳ P1 待办（配置级，见 data_service_main 注入点） |
| 6 | dirty 容量上限背压 | ⏳ P1 待办 |

验证：OFF 全量构建零警告，13/13 测试全绿（cache/version/sync demo 均过）。MySQL stmt 缓存为 MODULES=ON 路径，语法经静态核对，待 CI 真编译确认。
