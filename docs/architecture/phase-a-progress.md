# 阶段 A 进展 — 彻底优化基础设施 (Phase A Progress)

> 目标（用户原话）：在尽量小的内存、尽量低的运算能力、尽量窄的带宽下实现全部功能。
> 路线：推翻 v1.0 传统脚手架，把 v3.0 设计蓝图（ADR-011~016）真正建出来 + 补五个空白领域。
> 总纲：`docs/architecture/radical-optimization-blueprint.md`

## 已完成交付（OFF 构建可验证，17/17 ctest 全绿，含 W1–W5）

### W1 — Cap'n Proto 统一协议（带宽↓30-50%, CI 依赖减半）
- **10 个 schema 文件**：`proto/capnp/{common,login,movement,aoi,combat,quest,economy,social,envelope,data_service}.capnp`
- 替代原 FlatBuffers(9 .fbs) + Protobuf(22 .proto) 双轨，统一为单一零拷贝紧凑协议
- 每文件 `$Cxx.namespace("cami")` 避免跨文件命名冲突
- `scripts/capnp_lint.py`：本地无 capnp 编译器时的结构护栏（括号平衡/重复 ordinal/文件 ID）
- 校验结果：10/10 schema 结构通过，0 错误
- 旧 `proto/data_service.capnp`（未跟踪样例）已移除，归并至 `proto/capnp/`

### W2 — Arena 分配器 + SPMC 无锁队列（内存可控, CPU 零锁竞争）
- `common/arena.h`：请求级统一内存池，page 链式增长 + 8 字节对齐 + Reset（帧末尾零释放开销）
- `common/spmc_queue.h`：单生产者多消费者无锁队列（SEQ_CST 内存序 + 保守退避），热路径零互斥锁
- `tests/unit/arena_spmc_test.cpp`：**9 子测试全绿**
- 收益：消除散落 `malloc/free`、消息队列从 mutex 队列 → 无锁；直接命中红线"无全局锁"

### W3 — ECS SoA 组件层（计算 cache 友好, 零虚表）
- `game/ecs/components.h`：9 个 POD/trivially-copyable 组件（Transform/Velocity/Health/Mana/Level/CombatState/Faction/NetworkId/AiState）
- 编译期 `static_assert` 锁定全部组件 size 对齐（SoA 容量可精确计算）
- `game/ecs/ecs_world.h`：Entt 世界封装（门控 `CAMI_BUILD_MODULES`，与 D6 同模式——重型依赖交 CI）
- `tests/unit/ecs_component_test.cpp`：3 子测试（TriviallyCopyable / NoHiddenOverhead / SoAContiguousIteration）全绿
- `game/ecs/CMakeLists.txt`：OFF 编译组件层 + 门控 Entt 世界测试
- 根 `CMakeLists.txt`：修复 `find_package(GTest/Threads)` 顺序（前移到 `add_subdirectory` 之前，否则子目录测试找不到 GTest 目标）

## 根因修复记录
- **GTest 动态库 PATH 问题**：MinGW 下 GTest 为 MSYS2 动态库，直接运行 exe 报 `libgtest_main.dll not found`；须 `export PATH="/c/msys64/mingw64/bin:..."` 后运行/ctest（已在所有命令前缀固化）
- **测试断言对齐**：`NetworkId` 含 uint64 致 8 字节对齐 → `sizeof==16`（非 12），改为 `static_assert(sizeof==16)` 准确表达

## W4 — RocksDB 嵌入式 KV 主力 + 事件溯源（接口骨架已落地, MODULES 门控）
- **RocksDBBackingStore**（`data/rocksdb_proxy/rocksdb_backing_store.{h,cpp}`，门控 `CAMI_BUILD_MODULES` + vcpkg rocksdb）：
  实现与 MySQLBackingStore 同构的 `BackingStore` 五接口（Load/Store/LoadWithVersion/CasStore/Delete），
  三列族 `state/meta/events`：state=物化状态、meta=64 位版本裁决源（**分桶互斥锁 64 桶, 非全局锁**, 满足红线）、
  events=追加不可变事件（WAL 持久, 支持 `AppendEvent`/`RebuildFromEvents` 事件溯源）。
  直接消灭 Dev-Std 的 MySQL+SS+Redis 外部栈 → Dev-Mini 收敛为单进程嵌入式 KV（≈数百 MB）。
- **自主存储治理器 StorageRouter**（`data/rocksdb_proxy/storage_router.h`, 纯 STL / OFF 可编译, 本角色核心交付）：
  三态机 `Shadow → ActivePrimary → FallbackOnly`。
  ① 影子部署：候选不服务, 写入双写、读取比对, 静默评估正确性/延迟/资源, 绝不冒险切主；
  ② 熔断：候选连续失败达阈值（默认 5）立即隔离 + 全量回退基线 + 告警, 杜绝失控环路（类比 token 耗竭防护）；
  ③ 自主晋升：影子期 ops 达标 且 偏差率<=上限 且 延迟软门槛内 → 自动切主。
  晋升硬闸门 = 正确性(零偏差) + 健康(错误率<1%)；延迟仅软门槛（容忍 2x）——迁移收益是**消灭外部栈的总成本下降**, 非单 op 延迟优于网络 MySQL。
  持续双向比对, 任何回退立即熔断。
- **单测** `tests/unit/storage_router_test.cpp`（5 用例全绿: 影子服务基线+晋升 / 偏差计数 / 熔断跳闸 / CAS 双写一致 / 活跃主降级）, 用 InMemoryStore + ControllableStore 仿真双端。`OFF 构建 16/16 ctest 绿`。
- **CMake/vcpkg**：`data/CMakeLists.txt` 加 `find_package(RocksDB CONFIG QUIET)` 门控后端（双 target 名兜底 `RocksDB::rocksdb`/`rocksdb`）；`vcpkg.json` 加 `rocksdb`。
- ⚠️ `rocksdb_backing_store.cpp` 真编译需 `build-modules-on` CI（本地无 vcpkg/出网），target 名以安装输出为准。

## W5 — QUIC 网络层骨架 + 连接迁移治理器（接口已落地, MODULES 门控）
- **传输无关抽象 `ITransport`**（`gateway/transport/transport.h`, 纯 STL / OFF 可编译）：
  统一 `ConnectionId`(16 字节不透明) + `MigrationResult` + `Start/Stop/Send/Recv/Migrate` 最小接口。
  解耦网关连接层与具体协议，使现有 TCP 路径可作"基线"、QUIC 作"候选"同构治理。
- **QUIC 候选后端 `QuicTransport`**（`gateway/transport/quic_transport.{h,cpp}`, 门控 `CAMI_BUILD_MODULES` + `find_package(msquic)`）：
  头文件刻意不引入 msquic 类型（句柄经 `Impl` 不透明指针持有），OFF 下零重型依赖；
  真绑定（0-RTT + ConnectionID 无状态迁移）交 msquic bootstrap 后补全。
  直接命中架构红线"GameNode 宕机 800ms 内完成玩家连接上下文迁移"——且无状态网关水平扩展（任意网关凭 ConnectionID 接管，无需 SO_REUSEPORT 绑定）。
- **连接迁移治理器 `ConnectionMigrator`**（`gateway/transport/connection_migrator.h`, 纯 STL / OFF 可编译, 本角色护栏核心交付）：
  三态机 `Shadow → ActivePrimary → FallbackOnly`，与 W4 StorageRouter 同构：
  ① 影子部署：候选不承载真实迁移，仅对采样连接做影子迁移，静默评估成功率/延迟/资源；
  ② 熔断：候选连续迁移失败达阈值（默认 5）立即隔离 + 强制回退 TCP 基线 + 告警，杜绝故障候选持续吞流量；
  ③ 自主晋升：影子期评估达标（成功率≥99% 且 连续失败=0 且 p99 延迟≤800ms SLA）后自动切主。
  晋升硬闸门 = 正确性(成功率) + 架构红线(800ms SLA)；迁移收益是消灭 TCP 重连 + 支持无状态网关扩展，单跳延迟本就远低于 SLA。
- **仿真传输 + 单测** `gateway/transport/in_memory_transport.h` + `tests/unit/connection_migrator_test.cpp`（6 用例全绿：
  影子路由基线 / 影子评估计数 / 达标晋升 / 熔断跳闸 / 活跃主降级兜底 / FallbackOnly 强制基线），用 InMemoryTransport 仿真双端，不依赖 Boost/真实 socket。
- **CMake**：`gateway/transport/CMakeLists.txt` 注册 INTERFACE lib `cami_gateway_transport`（header-only）+ 单测 + `find_package(msquic QUIET)` 门控后端；`gateway/CMakeLists.txt` 已 `add_subdirectory(transport)` 并 PUBLIC 链接。**OFF 构建 17/17 ctest 绿**。
- ⚠️ QUIC 真绑定需 `vcpkg.json` 加 `msquic` + bootstrap（端口待确认），CI 真编译验证；现有 TCP 连接/连接管理器路径保持不动，迁移治理器待网关采用 `ITransport` 后接入。

## 待办（迁移 + vcpkg bootstrap, 交 CI）
- **网络层真绑定**：`vcpkg.json` 加 `msquic`，补全 `QuicTransport` 0-RTT 无状态迁移实现，CI 真编译（W5 骨架已落地，仅待此类真绑定）。
- **迁移接入**：网关 `connection` 模块采用 `ITransport`，把 `ConnectionMigrator` 挂到 GameNode 故障转移 / 客户端换网路径（W5 骨架已就绪，仅待集成缝）。
- **玩法模块迁移**：各玩法模块（combat/aoi/quest/...）从单体逻辑迁移至 ECS SoA + Cap'n Proto 消息。
- **vcpkg 依赖**：`vcpkg.json` 已加 `capnproto` + `entt` + `rocksdb`，bootstrap 后 CI 真编译验证（RocksDB/Entt/QUIC/MySQL stmt 等 MODULES=ON 重型依赖本地不可验）。

## 资源收益预估（相对 v1.0 脚手架）
| 维度 | v1.0 | 阶段 A 目标 | 机制 |
|------|------|------------|------|
| 带宽/消息 | FlatBuffers+Protobuf 双轨 | Cap'n Proto 单一 | ↓30-50% |
| 内存分配 | 散落 malloc | Arena 池化 | 可预测、零碎片 |
| 队列锁 | mutex 队列 | SPMC 无锁 | 零竞争 |
| 实体迭代 | 虚表/AoS | ECS SoA POD | cache 友好 |
| 数据层外部依赖 | MySQL+SS+Redis 30容器≈12GB | RocksDB 嵌入式单进程 | 外部栈全消、↓内存 |
| 连接迁移/网关扩展 | TCP 重连 + SO_REUSEPORT 绑定 | QUIC 0-RTT + ConnectionID 无状态迁移 | 800ms 红线、无状态水平扩展 |
| CI 依赖 | grpc+protobuf+flatc | capnproto | 减半 |
