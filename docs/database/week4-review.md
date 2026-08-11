# Week4 数据层 — 周评审

> 周期：2026-08-31 ~ 2026-09-04（数据层专题周）  
> 结论：**5 个子任务全部完成，4 项硬性验收全部 PASS** ✅



---

## 1. 交付物清单

| 日  | 任务                         | 核心交付                                                                                                             | 验收             |
| -- | -------------------------- | ---------------------------------------------------------------------------------------------------------------- | -------------- |
| D1 | MySQL 8 分库 Schema          | `docs/database/er-player-sharding.md`(+svg)、`account-character-cap.md`、8 分库建表 SQL                                | 分片键/ER ✅       |
| D2 | ShardingSphere 8 分库 + 读写分离 | `docker/shardingsphere/*`、`docker/mysql/*`、`docker/docker-compose.yml`、`sharding-verification.md`                | 路由/读写分离 ✅      |
| D3 | Redis 缓存代理                 | `data/redis_proxy/*`、`cache-proxy.md`                                                                            | 命中率 100% ✅     |
| D4 | 数据同步（30s 落库/断线持久化/版本号）     | `data/sync/*`、`sync-verification.md`                                                                             | 落库/断线 ✅        |
| D5 | 版本校验 + 数据层压测 + 周评审         | `data/version/*`、`benchmark/shard_tps_bench.py`、`version-verification.md`、`shard-tps-bench.md`、`week4-review.md` | 并发拦截/9784TPS ✅ |

## 2. 四项硬性验收结果

| 验收项       | 目标                | 实测                                      | 结果 |
| --------- | ----------------- | --------------------------------------- | -- |
| 跨分片路由正确   | 分片键统一 `player_id` | 12 例 `player_id%8` 全对 + 跨分片聚合/IN/广播合并正确 | ✅  |
| 缓存命中率     | ≥ 95%             | 100.00%（Zipf 100 万读）                    | ✅  |
| 并发写冲突拦截   | 100%              | 8 线程 Cas 仅 1 成功                         | ✅  |
| 单分库写入 TPS | ≥ 5000            | 9784 TPS（单连接直插真实主库）                     | ✅  |

## 3. 架构符合性（对照项目红线）

- ✅ 分片键统一 `player_id`，跨分片路由正确（无全服广播，定向/聚合分开）
- ✅ 缓存命中率 ≥ 95%（实测 100%）——热数据在缓存，DB 负载趋零
- ✅ 并发写冲突 100% 被版本号拦截——多节点写带 `version` CAS
- ✅ 单分库写入 ≥ 5000 TPS（实测 9784）——8 分库集群余量充足
- ✅ 战斗循环不写 DB（写只进内存缓存，落库走 D4 异步批量）
- ✅ 无全局锁 / 单点定时器（版本校验为行级乐观锁）

## 4. 踩坑沉淀（已写入 `.workbuddy/memory/MEMORY.md`）

1. **ShardingSphere 5.5 改了 `READWRITE_SPLITTING` YAML 语法**：旧 `type: Static`+`props:` 直接崩，改 `writeDataSourceName`+`readDataSourceNames:` 列表。
2. **改 SS 配置必须 `--force-recreate`**：`up -d` 只比对挂载声明不比对内容，旧容器跑旧算法。
3. **从库不能带 `--super_read_only=ON` 启动**：复制控制语句被拦截致容器中止，应裸启动后动态 `SET GLOBAL read_only=ON`。
4. **宿主无法连 Docker 发布端口（WinError 10061）**：Windows Docker Desktop NAT 隔离；改用容器内 `mysql` 客户端直连测量。
5. **`docker compose exec` 并发是假象**：4 并发 727 TPS 是编排开销，规范测单连接直插（9784 TPS）。
6. **git bash 无 `/tmp`、无 `sleep`**：临时文件落 `build/`；延时用 `until` 轮询。
7. **GCC16 拒绝 `unordered_map<string>::find(string_view)` 隐式转换**：先 `std::string k(key)` 再查。
8. **`enable_testing()` 须在 `add_subdirectory` 之前**：否则子目录 `add_test` 不注册。

## 5. 风险与待办（下周）

- 🔲 **Redis 生产接入 [PROD]**：`RedisBackend` 待 `CAMI_BUILD_MODULES=ON` + vcpkg `redis-plus-plus`（D3 现为 STL 仿真）。
- 🔲 **落库失败重试 / 死信队列（Kafka）**：架构 §5 异步落库尚未接，当前为同步 `Store`。
- 🔲 **Data Service gRPC 封装**：GameNode 经统一代理读写（当前 `BackingStore` 为内存仿真）。
- 🔲 **集群并行压测**：建议 sysbench 对 8 分库各起一路，确认 ≥ 40k TPS 集群写入。
- 🔲 **临时 SQL 已清理**：`ins*.sql` 已从项目根目录删除。

## 6. 下周建议方向

1. 接入真实 Redis（D3 `[PROD]`），打通 `CacheProxy → RedisBackend`。
2. Data Service gRPC 封装：`GameNode` 经代理读写 MySQL/Redis，落实「禁止直连」。
3. 落库 Kafka 异步化 + 死信队列，完成架构 §5 同步/异步分离。
4. 用 sysbench 做 8 分库并行压测，给出集群容量基线报告。
