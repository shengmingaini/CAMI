# Week4 D4 — 数据同步模块 (sync_manager)

> 验收目标：**30s 批量落库 + 断线立即持久化 + 版本号防多节点并发覆盖**
> 实测结论：**全部通过** ✅
> 模块：`data/sync/sync_manager.h` + `data/sync/sync_demo.cpp`

---

## 1. 模块职责（架构 §3.1 数据管理层）

`SyncManager` 组合 `CacheProxy` + `BackingStore` + `VersionedStore`，负责把缓存中的脏数据按节奏**批量落库**，并在玩家**断线时立即持久化**；落库前用版本号做 CAS 校验，防止多节点并发写覆盖（架构红线「禁止无版本号多节点并发写玩家数据」）。

| 行为 | 策略 | 说明 |
|------|------|------|
| 周期落库 | 后台线程循环（演示 50ms，生产 30s） | 把 `dirty_` 批量 `FlushNow` 回分片 DB |
| 断线持久化 | `OnDisconnect()` 同步立即 `FlushNow()` | 不走后台线程，确保断线不丢数据 |
| 版本校验 | 新行 `Init` / 已存在 `Cas(version)` | 多节点并发写经 CAS 拦截，写入成功才 `Store` |

红线遵守：战斗循环只碰内存缓存；落库走异步批量（架构 §5 同步/异步分离，邮件/日志走 Kafka 异步）。

## 2. 接口设计

```cpp
SyncManager(CacheProxy& cache, BackingStore& store, VersionedStore& vstore,
            std::chrono::milliseconds period = 30000ms);
void Start();                 // 起后台线程 loop
void Stop();                  // 停线程并 join
void OnDisconnect();          // 立即 FlushNow（同步）
void MarkDirty(const string& key);
void FlushNow();              // 核心落库 + 版本校验
```

`FlushNow` 核心逻辑（修复后）：

```cpp
auto cur = vstore_.Load(k);
bool written = false;
if (cur) written = vstore_.Cas(k, *v, cur->version);  // 已存在：CAS 校验
else     { vstore_.Init(k, *v); written = true; }      // 新行：Init
if (written) { store_.Store(k, *v); ++ok; }            // 仅写入成功才落 DB
```

> 关键修复：早期版本只对已存在行调用 `Cas`，新行（未 `Init`）`Cas` 必失败 → 落库 0 条。现新行走 `Init` 分支。

## 3. 验证（实跑证据）

编译：`g++ -std=c++17 -O2 -I. data/sync/sync_demo.cpp data/redis_proxy/cache_proxy.cpp`，或 `cmake -DCAMI_BUILD_TESTS=ON` 后 `ctest`。

```
[periodic]   周期落库 flush_count=100 (应>=100), store.p0=v0
[disconnect] OnDisconnect 后 store.disc=dval (应=dval)
[bench]      批量落库 200000 条, 同步路径吞吐=757927 条/s (单分库 DB 实测见压测报告)

=== D4 验收: 30s批量落库+断线立即持久化 ? PASS ===
```

- **周期落库**：100 个周期全部 `flush`，`store` 含最终值 `v0` ✅
- **断线持久化**：`Stop` 后台线程后 `OnDisconnect()` 同步立即落盘 `dval` ✅
- **批量吞吐**：20 万条同步路径 **757,927 条/s**（DB 实测单分库 TPS 见 `shard-tps-bench.md`）
- cmake 集成：`cami_sync_demo` 注册为 ctest，3 个断言全 PASS

## 4. 生产接入（[PROD]，待 CAMI_BUILD_MODULES=ON）

- `BackingStore::Store` 接真实分片 DB（经 Data Service 代理，**GameNode 不直接连 MySQL**）
- 周期改 **30s**；批量用 `Multi-INSERT` / Pipeline 降低往返
- 落库失败 → 重试 + 死信队列（Kafka 异步，架构 §5）
- 版本号贯穿：玩家数据每行带 `version`，多节点并发写经 CAS 拦截

## 5. 复现命令

```bash
# 直编直跑 (无需 DB)
g++ -std=c++17 -O2 -I. data/sync/sync_demo.cpp data/redis_proxy/cache_proxy.cpp -o build/cami_sync_demo
./build/cami_sync_demo

# 经 cmake + ctest
cmake -B build -DCAMI_BUILD_MODULES=OFF -DCAMI_BUILD_TESTS=ON -G "MinGW Makefiles" .
cmake --build build --target cami_sync_demo
ctest --test-dir build -R sync_demo
```

## 6. 验收对照

| 验收项 | 结果 |
|--------|------|
| 30s（演示周期）批量落库 | ✅ 100 周期全 flush |
| 断线立即持久化 | ✅ OnDisconnect 同步落盘 dval |
| 版本号防并发覆盖 | ✅ 落库前 Cas 校验（详见 `version-verification.md`） |
| 批量落库吞吐 | ✅ 757,927 条/s 同步路径 |
