# D6 生产接入交付状态报告（Week4 → Week5 衔接）

> 生成时间：2026-08-11 21:24（本地会话收尾）
> 角色：数据库设计调优专家（先测量后优化 / 零停机 / 红线圈禁）
> 范围：用户 4 项生产构建任务（Redis 真接入 / Data Service gRPC / Kafka 异步+死信 / 8 分库 sysbench）

---

## 0. 结论速览

| 任务 | 代码状态 | OFF 构建 | MODULES=ON 真后端 | 沙箱实测 |
|------|----------|----------|-------------------|----------|
| ① 真实 Redis 接入 (D3 `[PROD]`) | ✅ 完成 | ✅ 绿 | ⏳ 待验 | ⛔ 不可达 |
| ② Data Service gRPC 封装 | ✅ 完成 | ✅ 绿 | ⏳ 待验 | ⛔ 不可达 |
| ③ Kafka 异步落库 + 死信队列 | ✅ 完成（桥接闭环） | ✅ 绿 | ⏳ 待验 | ⛔ 不可达 |
| ④ 8 分库并行 sysbench 基线 | ✅ runner 完成 | n/a | n/a | ⛔ 不可达 |

**架构红线符合性**：全部模块 `#ifdef CAMI_BUILD_MODULES` 门控，GameNode 经 `DataClient`(gRPC stub) 读写、**禁止直连 MySQL**（红线✅）；落库走 Kafka 异步 + CAS 版本号并发保护（红线✅）；战斗循环零 DB 写入、邮件/日志异步（架构 §5 同步异步分离✅）。

---

## 1. 四项交付物清单

### ① 真实 Redis 接入 — `data/redis_proxy/redis_backend.{h,cpp}`
- `RedisBackend : public CacheBackend`（redis-plus-plus，`sw::redis::Redis`）。
- 接口：`Get / Put(key,val,ttl) / Delete / Contains / Size` + `MGet / MPut`（pipeline 批量）。
- `Put` 强制 TTL（防冷数据常驻内存）、缓存双删策略接口预留。
- 打通 `CacheProxy → RedisBackend`，替换 D3 的 `InMemoryBackend` 占位。

### ② Data Service gRPC 封装 — `proto/data_service.proto` + `data/data_service/`
- `DataService` 服务：`Get / Put / Cas / BatchPut / Delete`（`PlayerRow{player_id,payload,version}`）。
- **服务端** `data_service_impl.{h,cpp}`：Read-Through / Write-Back+Through，`Put`/`Cas` 全部走 `VersionedStore` 版本号保护（并发写玩家数据红线✅）。
- **GameNode 侧** `data_client.{h,cpp}`：gRPC stub 封装，**GameNode 不直接持 DB 连接**（红线✅）。
- `data_service/CMakeLists.txt`：protoc 代码生成（`data_service.pb.*` + `data_service.grpc.pb.*`）。
- `data_service_main.cpp`：进程装配 RedisBackend + MySQLBackingStore + VersionedStore + CacheProxy + KafkaSinkWorker（env-var 配置）。

### ③ Kafka 异步落库 + 死信队列 — `data/sync/kafka_flush.{h,cpp}`
- `KafkaFlush`（生产者）：脏行 `FlushMessage{key,value,version}` 发布到落库 topic；`BLOCK_ON_FULL_QUEUE` 背压。
- `KafkaSinkWorker`（消费者）：`enable.auto.commit=false`，**仅持久化成功才提交 offset**（at-least-once，不丢）；
- 持久化失败 → 转入 `<topic>.DLQ` 死信队列，可独立重试，不阻塞主链路。
- **生产者桥接（D6-T3 收口，2026-08-12）**：`CacheProxy::FlushDirty()` 现支持注入 `AsyncFlushSink`；`data_service_main.cpp` 注入 `KafkaFlush::PublishBatch`，起 **30s 周期落库线程** 把 WriteBack dirty 排干到 `cami.player.flush`，`SIGINT/SIGTERM` 触发优雅停机 + 最终落库兜底。完整链路见 `d6-code-review.md` 第七节。
- 落实架构 §5「邮件/日志/Kafka 异步队列」与「禁止战斗循环内写 DB」。

### ④ 8 分库并行 sysbench 基线 — `benchmark/sysbench/run-8shard.sh`
- 每分库 `oltp_insert`（端口 33060+i，经 ShardingSphere 代理或直连主库），并行拉起 8 路后汇总 TPS → `results/cluster_summary.txt`。
- 单分库基线 **9784 TPS**（W4-D5 容器内单连接直插实测，≥5000 PASS）；8× 投影 **≈78,272 TPS** 容量基线。
- ⚠️ 容器内 `docker compose exec` 并发 4 路得 727 TPS 是**编排开销假象**，非 DB 上限，规范测单连接直插。

---

## 2. 构建验证证据（本会话复核）

### OFF（STL-only）路径 — 绿 ✅
- 此前摘要末步报 "OFF include check FAILED" 但错误日志缺失；本会话复跑定位根因：
  1. 旧命令误引用**不存在**的 `data/version/version.cpp`（`VersionedStore` 与 `InMemoryBackend` 均 header-only，无需 .cpp）；
  2. safe-delete 把被吞命令里的 `rm -f` 执行掉、删了临时文件 → 后续编译 "No such file"。
- 用正确源复跑：`g++ -std=c++17 -I. off_include_check.cpp data/redis_proxy/cache_proxy.cpp` → **GCC_RC=0**。
- 与先前 `cmake -DCAMI_BUILD_MODULES=OFF -DCAMI_BUILD_TESTS=ON` 配置/编译 exit 0 + **ctest 12/12 通过** 一致 → OFF 轻量路径干净，PROD 门控文件作为空 TU 不影响。

### MODULES=ON（真实后端）— CI 门禁已就绪，待 push dev 触发实跑 ⏳→🟡
- 沙箱无 vcpkg 出网（GitHub 不可达）、Docker 不可达 → 真实 Redis/gRPC/Kafka/MySQL 编译与运行**仍不可在本环境闭环**。
- **CI 门禁已就绪**：`.github/workflows/ci.yml` 的 `build-modules-on` job（ubuntu:24.04 容器 + vcpkg 全量 manifest + Redis Cluster 服务 + `ctest`）即 MODULES=ON 真编译门禁，覆盖 `data_service_main` / `kafka_flush.cpp` / `mysql_backing_store.cpp` / `cache_proxy.cpp`(async sink) 等 D6 生产代码的真实编译+链接。
- **本次修复一处会导致该 job 红的链接 bug**：`data/CMakeLists.txt` 原把 `redis++`/`cppkafka`/`libmysql` 链成 `PRIVATE`，而 `cami_data_service_main` 直接实例化 `RedisBackend`/`MySQLBackingStore`/`KafkaFlush` 具体类，静态库 PRIVATE 依赖不向下传递 → main 链接报未定义符号。已改 `PUBLIC`（生产后端属 cami_data 公开契约，下游部署程序合法实例化）。
- 给 CI 加了 `workflow_dispatch` 手动触发入口（便于单独重跑重型 job）。
- **仍待**：push 到 `dev`（或 PR / 手动触发）让 CI 实际跑一次确认绿；Kafka 端到端 + DLQ 幂等仍需 staging 实演（CI 仅覆盖编译，未起 Kafka/MySQL 服务做运行时验证）。

---

## 3. 待用户确认 / 待办（明确移交）

### A. CI / 本机验证 MODULES=ON 真后端
- **CI 已固化验证路径**：`build-modules-on` job 用 vcpkg manifest（全量 7 依赖）真实编译+链接 D6 生产代码。vcpkg target 名是否找得到、包能否装上，由 CI 直接暴露（红=有问题），不再需要本机单独核对 `docs/database/data-service.md` §4 的待确认列表（F1 已将 grpc/protobuf/cppkafka/libmysql 加回 vcpkg.json；`gRPC::gRPC++` 等 target 名已在 QUIET 守卫下确认）。
- **本地 MinGW 仍受限**：`libmysql` 不支持 mingw（vcpkg 拒绝），本地即使装了 grpc/protobuf/cppkafka，缺 libmysql 时 `data_service_main` 仍链接失败（它直接实例化 `MySQLBackingStore`）。本地 MinGW 仅能跑 OFF；MODULES=ON 真编译以 **CI Linux 为权威**。
- 下方本机命令仅供参考（需联网装全量依赖，MinGW 可能卡在 libmysql）：

**本机命令（需联网装依赖）**：
```bash
cmake -B build-mod -DCMAKE_BUILD_TYPE=Release \
  -DCAMI_BUILD_MODULES=ON -DCAMI_BUILD_TESTS=ON \
  -DCMAKE_TOOLCHAIN_FILE=vcpkg_mingw.cmake \
  -DBOOST_ROOT=C:/msys64/mingw64 -G "MinGW Makefiles"
cmake --build build-mod -j4
ctest --test-dir build-mod --output-on-failure
```
> Win+MinGW 强制 triplet 已写进 `CMakeLists.txt`（`x64-mingw-static` FORCE），无需命令行传。无 Docker 时 RedisCluster 真实用例 GTEST_SKIP 属正常。

### B. 跑 8 分库并行 sysbench
需 Docker（SS 代理 + 8 主库就绪）+ 宿主装 `sysbench`：
```bash
cd benchmark/sysbench && bash run-8shard.sh
# 产出 benchmark/sysbench/results/cluster_summary.txt
```
填实 `cluster-capacity-baseline.md` 的并行实测值（当前仅单分库基线 + 8× 投影）。

### C. 临时文件清理（本会话已完成大部分）
- 已用绝对路径 `rm -f` 删除：`off_include_check.cpp / off_check.exe / off_compile.log / ins*.sql`（5 个 week4 临时 SQL）。
- `build-*` 系列目录（`build-ci-off / build_d3 / build-check / build-mod` 等）均已被 `.gitignore` 覆盖（本地杂余、非仓库污染）；safe-delete 拦截 bulk `rm -rf`，故未强删，用户可 `git clean -fdx` 自清。
- 残留 `NUL`（Windows 保留设备名，0 字节，git 也提交不了）无害，忽略。

---

## 4. 风险与红线提醒（专家视角）

1. **未实测即不可声称「通过」**：MODULES=ON 真实编译、8 分库并行 TPS、Kafka DLQ 端到端、gRPC 跨进程延迟——这四项**均未在真实环境跑过**，当前仅静态代码 + OFF 构建绿。上线前必须在非生产闭环。
2. **vcpkg target 名是最高频坑**：`find_package` 名 ≠ port 名（redis-plus-plus→redis++ 已踩过）。gRPC/protobuf/cppkafka/libmysql 加回 vcpkg.json 后，务必以 `vcpkg install` 输出的 "provides CMake targets" 提示为准改 `CMakeLists.txt`。
3. **Kafka 至少一次语义**：消费者必须幂等或去重——`Cas(version)` 已天然幂等（version 不匹配直接拒），DLQ 重试不会造成脏写，但**首次生产部署仍需实演 DLQ 重试路径**。
4. **money/精度**：玩家货币沿用 `BIGINT` 整数最小单位（schema.sql 已定），落库代理不得引入浮点。
5. **连接迁移零停机**：Data Service 作为独立进程，重启需配合 GameNode 重连（连接迁移设计 `docs/design/connection-migration.md` 800ms 预算），勿在高峰期裸重启。

---

## 5. 本会话做了什么
- 复核并**确认 OFF 头包含 sanity 通过（GCC_RC=0）** —— 澄清了摘要末步的 "FAILED/日志缺失" 假象（根因：误引不存在的 version.cpp + safe-delete 误删临时文件）。
- 清理 week4/本会话临时文件（绝对路径 `rm -f`），确认 `build-*` 已被 gitignore。
- 更新项目记忆：safe-delete 路径格式坑 + D6 模块清单 + 待验项。
- 本文档即 D6 交付状态收口。

## 6. 后续会话补完：Kafka 生产端桥接闭环（2026-08-12）
- **补齐 D6-T3 缺失的 glue**：`CacheProxy` 增 `AsyncFlushSink` + `SetAsyncFlushSink`（纯 STL，OFF 安全）；`FlushDirty()` 重写（注入 sink 走 Kafka 发布，否则同步回退）；`data_service_main.cpp` 注入 `KafkaFlush::PublishBatch` + 30s 周期落库线程 + SIGINT/SIGTERM 优雅停机（含最终落库兜底）。
- 形成完整异步落库闭环：GameNode→DataClient(gRPC)→CacheProxy.WriteBack(dirty)→30s FlushDirty→KafkaFlush→Kafka topic→KafkaSinkWorker→Cas+MySQL 落库。GameNode 仍不直连 MySQL（红线✅）。
- **OFF 构建复跑：ctest 12/12 通过**（demo 下 sink 为空走同步回退，向后兼容零回归）。
- 仍待：`MODULES=ON` 真编译（CI Linux `build-modules-on`）+ Kafka 端到端实演 + DLQ 幂等复核。详细见 `d6-code-review.md` 第七节。
- **CI 门禁收尾（2026-08-12 补）**：`data/CMakeLists.txt` 把 `redis++`/`cppkafka`/`libmysql` 由 `PRIVATE` 改 `PUBLIC`，修复 `cami_data_service_main` 因静态库依赖不向下传递导致的链接未定义符号（否则 `build-modules-on` 红）；`ci.yml` 加 `workflow_dispatch` 手动触发入口。OFF 复跑仍 12/12 零回归。
- **全面复查 + 11 处修复（2026-08-12 第二轮收尾）**：逐文件复查 D6 生产代码，修 2 bug（Kafka DLQ offset 卡死重放、`CacheProxy::Delete` 删除语义失效）+ 3 一致性（`Put` 版本静默失败→新增 `VersionedStore::Set`、驱逐 key 无限重试、信号处理器非安全调 Shutdown）+ 5 健壮（`MGet` 真 MGET、gRPC deadline、MySQL 重连/超时/注入白名单、停机 cv 即时唤醒）。2 项架构级发现（多副本 CAS 未下沉 MySQL、缺 Health RPC）待拍板。详见 `d6-code-review.md` 第八节。⚠️ **全部 Week4-D6 工作仍未 commit，建议尽快提交。**
