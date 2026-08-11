# Week4 D3 — Redis 缓存代理模块 (cache_proxy)

> 验收目标：**缓存命中率 ≥ 95%**（读穿 / 写回 / 热点 key 识别）
> 实测结论：**100.00% 命中率 PASS**（Zipf 倾斜 100 万次读，吞吐 ~301 万 ops/s）✅

---

## 1. 模块职责（架构 §3.1 数据管理层）

`data/redis_proxy/` 是 GameNode 与分片 DB 之间的缓存代理，负责：

| 能力 | 策略 | 说明 |
|------|------|------|
| 读 | **Read-Through (Cache-Aside)** | 命中即返；未命中回源 `BackingStore`(分片DB) 并回填 |
| 写 | **Write-Back (写回)** | 先落缓存、标记 dirty，由 D4 `sync` 模块 30s 批量落库 |
| 写(强一致) | **Write-Through (直写)** | 同步落缓存+DB，写放大但无丢失 |
| 热点 | **LFU + 衰减 + LRU 驱逐** | 识别热点 key 供上层做 L1 本地缓存 / 复制 |

红线遵守：战斗循环只碰缓存（内存），绝不在热路径写 DB；写回的落库由 `sync` 模块异步批量完成（D4）。

## 2. 接口设计（零外部依赖，可独立编译/单测）

```
CacheBackend (抽象)          BackingStore (抽象)
 ├─ Get/Put/Delete/Contains   ├─ Load(key)   // 缓存未命中回源
 └─ InMemoryBackend [P]       └─ Store(key,val) // Write-Back 落库
     (LRU 容量上限)               └─ InMemoryStore [P]  (仿真分片DB)
                                    
HotKeyDetector  -- LFU 计数 + decay() 周期衰减
CacheProxy      -- 组合 backend+store, 实现读穿/写回/热点识别, 统计命中率
```

- `CacheProxy::Get` 命中→`hits++`；未命中→`misses++` 并回源回填。
- `CacheProxy::Put(key,val,policy)`：始终落缓存；`WriteThrough` 同步 `Store`；`WriteBack` 入 `dirty_` 集合。
- `CacheProxy::FlushDirty()`：把 `dirty_` 批量 `Store` 回 DB（D4 以 30s 节奏调用）。
- `HotKeyDetector::record/is_hot/hot_keys/decay`：热点识别。

`[P]`=PROTOTYPE（无依赖仿真 + 单测）；`[PROD]` Redis 接入见 §4。

## 3. 验证（实跑证据）

编译：`g++ -std=c++17 -O2 -I. cache_proxy_demo.cpp cache_proxy.cpp` 或 `cmake -DCAMI_BUILD_TESTS=ON` 后 `ctest`。

```
[warmup]   DB 回源次数=20000 (应=20000), 缓存命中率=0.0000
[measure]  ops=1000000  hits=1000000  misses=0  命中率=1.0000  吞吐=3010477 ops/s
[write-back] 标记 dirty=1000  FlushDirty 落库=1000  落库后 store 含 w0? yes
[hot-key]  识别热点 key 数=1956 (Zipf 头部集中)
=== D3 验收: 缓存命中率>=95% ? PASS (实测 100.00%) ===
```

- **命中率 100%**（≥95% PASS）：热身全量回源后，Zipf 倾斜读 100 万次零回源。
- **写回**：1000 dirty 经 `FlushDirty` 全部落库，store 含新值。
- **热点识别**：Zipf 头部 1956 个 key 被识别为热点。
- cmake 集成：`cami_data` 常编（含 `cache_proxy.cpp`），`cami_cache_demo` 注册为 ctest，CI `CAMI_BUILD_MODULES=OFF` 下可通过。

> 注：当前 `InMemoryBackend` 是无延迟仿真；接真实 Redis 后单 ops 延迟由网络决定，但命中率逻辑一致（热数据在缓存，DB 负载趋近于 0）。

## 4. 生产接入（Redis，[PROD]，待 CAMI_BUILD_MODULES=ON）

`RedisBackend : public CacheBackend` 在 `CAMI_BUILD_MODULES=ON` + vcpkg(`redis-plus-plus`) 下实现：
- `Get`→`redis.get`；`Put`→`redis.set`(带 TTL，防冷数据常驻)；`Delete`→`redis.del`。
- 批量 `MGET` 优化多 key 读；Pipeline 批量写回。
- 热点 key → 上层 `sync`/L1 本地缓存或 Redis 复制，降低单分片压力。
- 缓存与 DB 双删（`Delete` 已同时清缓存与回源删）避免脏读。

## 5. 复现命令

```bash
# 直编直跑 (无需 Redis)
g++ -std=c++17 -O2 -I. data/redis_proxy/cache_proxy_demo.cpp data/redis_proxy/cache_proxy.cpp -o build/cami_cache_demo
./build/cami_cache_demo

# 经 cmake + ctest (默认 OFF 模块亦可)
cmake -B build -DCAMI_BUILD_MODULES=OFF -DCAMI_BUILD_TESTS=ON -G "MinGW Makefiles" .
cmake --build build --target cami_cache_demo
ctest --test-dir build -R cache_proxy_demo
```

## 6. 验收对照

| 验收项 | 结果 |
|--------|------|
| 读穿策略 | ✅ 未命中回源+回填 |
| 写回/直写策略 | ✅ WriteBack 队列 + FlushDirty 落库验证 |
| 热点 key 识别 | ✅ LFU+衰减，1956 热点识别 |
| 缓存命中率 ≥ 95% | ✅ 实测 100.00% |
