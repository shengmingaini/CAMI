# Week4 D6 — 数据层生产接入（Redis / gRPC Data Service / Kafka 异步落库）

> 目标：打通 `CacheProxy → RedisBackend`，实现 `GameNode 经 Data Service gRPC 代理读写 MySQL/Redis`（落实「禁止直连」），并完成架构 §5 同步/异步分离（Kafka 异步落库 + 死信队列）。
> 全部 `[PRODUCTION]` 代码 **门控在 `CAMI_BUILD_MODULES=ON`**（vcpkg 提供 redis-plus-plus / grpc / protobuf / cppkafka / libmysql），OFF 构建不受影响。

---

## 1. 架构闭环（红线：GameNode 禁止直连）

```
            ┌──────────────────────── Data Service (独立进程/多副本) ───────────────────────┐
            │                                                                            │
 GameNode ──┤  gRPC DataClient  ──▶  DataServiceImpl                                    │
 (逻辑层)   │     (禁直连)              │ 组合:                                            │
            │                          │  CacheProxy (读穿/写回)                           │
            │                          │   ├─ RedisBackend ─────▶ Redis Cluster           │
            │                          │   ├─ VersionedStore (乐观锁 CAS)                 │
            │                          │   └─ MySQLBackingStore ─▶ ShardingSphere 代理   │
            │                          │        (player_id%8 分片路由)                     │
            │                          │  KafkaFlush (写回队列) ──▶ Kafka topic          │
            └──────────────────────────┼─────────────────────────────────────────────────┘
                                        │                                     │
                                        │ 30s / 断线                           ▼
                                        │                              KafkaSinkWorker
                                        │                                (消费→CAS→Store)
                                        │                                     │ 失败
                                        │                                     ▼
                                        │                             死信 topic (.DLQ)
```

- **GameNode 不持有任何 MySQL/Redis 句柄**，只持 `DataClient`（gRPC stub）。
- 读：CacheProxy 读穿（命中 Redis → 未命中回源 MySQLBackingStore → 回填）。
- 写：默认 Write-Back（落 Redis + 进 Kafka 异步落库）；强一致场景用 Write-Through。
- 并发：所有写经 `VersionedStore.Cas` 乐观锁，100% 拦截多节点覆盖（D5 已验证）。

## 2. 模块落点

| 文件 | 内容 | 门控 |
|------|------|------|
| `data/redis_proxy/redis_backend.{h,cpp}` | `RedisBackend : CacheBackend`（Get/Put+TTL/Delete/Contains/MGet/MPipeline） | MODULES=ON (redis++) |
| `proto/data_service.proto` | gRPC `DataService`（Get/Put/Cas/BatchPut/Delete） | — |
| `data/data_service/data_service_impl.{h,cpp}` | 服务端：组合 CacheProxy+Version+Store | MODULES=ON (grpc/protobuf) |
| `data/data_service/data_client.{h,cpp}` | 客户端：GameNode 侧调用封装（强制代理） | MODULES=ON (grpc/protobuf) |
| `data/data_service/data_service_main.cpp` | Data Service 独立进程入口 | MODULES=ON |
| `data/mysql_proxy/mysql_backing_store.{h,cpp}` | `MySQLBackingStore : BackingStore`（libmysql → ShardingSphere） | MODULES=ON (libmysql) |
| `data/sync/kafka_flush.{h,cpp}` | `KafkaFlush` 生产者 + `KafkaSinkWorker` 消费者 + DLQ | MODULES=ON (cppkafka) |
| `data/data_service/CMakeLists.txt` | protoc 代码生成 + `cami_data_service` lib | MODULES=ON |
| `benchmark/sysbench/run-8shard.sh` | 8 分库并行压测 runner | — |

## 3. 编译 / 运行（需 vcpkg + 网络）

```bash
# 1) 安装依赖 (vcpkg manifest 自动拉取: redis-plus-plus/grpc/protobuf/cppkafka/libmysql/openssl/gtest)
#    注意: 重新引入 grpc 会触发较长 (20~40min) 源码构建, 首跑后走二进制缓存。
export VCPKG_ROOT=$HOME/vcpkg
git clone https://github.com/microsoft/vcpkg.git $VCPKG_ROOT   # SCP 形式绕过 insteadOf 墙

# 2) 配置 + 构建 (MinGW 需 x64-mingw-static triplet)
cmake -B build-mod -DCAMI_BUILD_MODULES=ON -DCAMI_BUILD_TESTS=ON \
      -DVCPKG_TARGET_TRIPLET=x64-mingw-static \
      -DCMAKE_TOOLCHAIN_FILE=$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake .
cmake --build build-mod -j

# 3) 跑 Data Service (环境变量注入连接)
CAMI_REDIS_URI=redis://redis:6379 \
CAMI_MYSQL_HOST=shardingsphere-proxy CAMI_MYSQL_PORT=3307 \
CAMI_MYSQL_USER=root CAMI_MYSQL_PASS=root CAMI_MYSQL_DB=cami_db \
CAMI_KAFKA_BROKERS=kafka:9092 \
CAMI_DATA_SVC_ADDR=0.0.0.0:50051 \
./build-mod/data/data_service/cami_data_service_main

# 4) GameNode 侧: 构造 DataClient("data-service:50051") 调用, 绝不直连 MySQL/Redis
```

## 4. ⚠️ 验证状态（重要）

- **OFF 构建（STL-only）已复跑 green**：`cami_cache_demo` / `cami_version_demo` / `cami_sync_demo` 三个 ctest 仍全 PASS（PROD 代码整体 `#ifdef CAMI_BUILD_MODULES` 守护，OFF 为空 TU）。
- **MODULES=ON 真实后端代码本沙箱无法编译验证**：agent runtime 无 GitHub/vcpkg 出网、Docker 不可达。下列项需在你本机或 `build-modules-on` CI 跑通后才算闭环：
  - `RedisBackend` redis++ API 调用（set 的 TTL 时长类型、MGet 实现）。
  - gRPC 代码生成（`protoc` + `grpc_cpp_plugin`）与目标名 `gRPC::gRPCCPP` / `protobuf::libprotobuf`。
  - `MySQLBackingStore` libmysql C 客户端目标名（vcpkg `libmysql` 提供 `libmysql` / `libmysql::libmysql`？构建时确认；`data/CMakeLists.txt` 已用裸名 `libmysql` 链接，必要时改为导入目标）。
  - `cppkafka` 目标名（`cppkafka`）。

## 5. 下一步

- MODULES=ON 在本机/CI 跑通，修正上述待确认点。
- GameNode 接入 `DataClient`（替换当前内存 BackingStore 桩）。
- 真实 Kafka 集群联调：发布→消费→落库→DLQ 全链路压测。
- 8 分库并行 sysbench 容量基线实测（见 `cluster-capacity-baseline.md`）。
