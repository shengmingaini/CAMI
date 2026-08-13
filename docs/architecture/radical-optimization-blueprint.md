# CAMI 彻底优化白皮书：最小资源实现全部功能

> **版本**: v4.0 | **日期**: 2026-08-12
> **目标**: 在最小内存、最低运算、最窄带宽下实现全部功能。
>   **不照搬 WoW 落后架构，采用合理高效架构与技术。**
>   **可从基础推翻现有结构进行彻底优化。**

---

## 1. 全局审视：资源浪费全景图

按三类核心资源对现状做逐层诊断（代码已验证 + 设计文档已覆盖但未实现）：

### 1.1 内存浪费

| # | 问题 | 根因 | 现状严重度 |
|---|------|------|-----------|
| 1 | `std::string` / `std::vector` 热路径频繁堆分配 | 无统一 Arena 分配器 | 每帧、每消息、每缓存读写都在 malloc |
| 2 | 对象池各自管理，无全局复用 | 每个模块自己 new/delete | 碎片化，无法跨模块共享空闲资源 |
| 3 | 配置全量加载（21 ConfigSet） | 启动全部解析进内存 | 已优化（懒加载），但数据本身仍是 Protobuf 描述符+JSON，膨胀 |
| 4 | MySQL buffer pool / Redis maxmemory | 关系型引擎元数据 + 双缓存层 | ~1GB 只存玩家在线数据（极低有效载荷比） |
| 5 | ECS 数据布局缺失 | 当前用虚函数 OOP，每个 Entity 独立分配 | Cache miss 高 |

### 1.2 CPU 浪费

| # | 问题 | 根因 | 现状严重度 |
|---|------|------|-----------|
| 1 | 线程上下文切换 | io_context 线程池（每连接一个线程亲和） | 5 万连接 = 数万线程切换/秒 |
| 2 | 锁竞争 | 数据层 3 把锁/读命中、网关层原有 2 把全局锁 | 网关已消除（本轮优化），数据层部分消除 |
| 3 | 序列化/反序列化 | FlatBuffers+Protobuf 双轨编码 | 高频消息每次序列化、配置数据多一次 JSON→PB 解析 |
| 4 | 虚函数调用 | BackingStore/CacheBackend 等抽象层 | 每个接口调用需虚表查址（非数据导向设计） |
| 5 | 字符串拷贝 | `std::string key` 在缓存热路径反复构造 | 每读一次 key 拷贝 |
| 6 | MySQL 网络 I/O | 每 30s 批量落库、prepare 未复用 | RTT 突发（已优化 prepare），但根本问题是"需要 MySQL" |

### 1.3 带宽浪费

| # | 问题 | 根因 | 现状严重度 |
|---|------|------|-----------|
| 1 | FlatBuffers 大体积 | 零拷贝代价：每条消息携带完整 schema 偏移表 | 比 Protobuf 大 30-50% |
| 2 | 高频消息无位压缩 | 移动/技能数据用 FlatBuffers 全字段编码 | 已设计（v2.0 轨道 A）但代码未实现 |
| 3 | 全量数据同步 | AOI 进出视野全量推送 | 已设计差分压缩（v3.0）但代码未实现 |
| 4 | gRPC 元数据开销 | 跨层通信 gRPC HTTP/2 头 + Protobuf 包装 | 共享内存 IPC 可省 |

---

## 2. 目标架构：五个维度的彻底重构

### 2.1 网络层：TCP+线程池 → QUIC+协程

**为什么推翻**：TCP 队头阻塞对 MMO 高频消息致命（一个丢包拖住整条流）。
QUIC 0-RTT 握手 + 多路复用 + 用户态拥塞控制，配合 C++20 协程实现**无上下文切换的并发**。

**目标方案**（落地 v3.0 ADR-014）：
- msquic / lsquic 库做底层 QUIC 传输
- boost::asio C++20 coroutine（`co_await`）替代线程模型：**一个协程 = 一个连接，无线程切换开销**
- 边缘网关就近接入（降低玩家 RTT 80%），中心集群内网专线转发

**迁移策略**：Gateway 层先双栈运行（TCP + QUIC 同进程监听两端口），客户端逐步切换。

### 2.2 计算层：单体 GameNode → Cell 网格 + ECS + 共享内存 IPC

**为什么推翻**：单体进程内各模块即使解耦，CPU 资源仍是竞争关系。
战斗循环 5ms 红线在单体进程里被 AOI/社交/任务无关负载挤压。

**目标方案**（落地 v3.0 ADR-011/012/016）：
- **Cell 分片**：开放世界切成 100m×100m Cell，每个 Cell 独立进程（独占 CPU 核心）
- **ECS SoA 布局**：实体数据按组件类型连续存储（Structure of Arrays），SIMD 友好，Cache 友好
- **共享内存 IPC**：同物理机 Cell 间通信走 `shm` + RingBuffer（延迟 0.02ms vs gRPC 1ms）
- **战斗进程独立隔离**：副本/团本 = 独立 Cell 进程，崩溃不影响世界

**迁移策略**：从副本/团本做起（独立 Cell，风险可控），验证稳定后推广到世界场景。

### 2.3 数据层：MySQL 分库分表 → 分布式 KV + 事件溯源

**为什么推翻**：WoW 用 MySQL 因为那是 2004 年的最佳选择。
现在有更优方案。MMO 玩家数据的本质是 **key-value**（player_id → serialized state），
关系型 schema 的"表连接/事务/回滚"对我们是**纯粹的浪费**。

**目标方案**：
- **L1 进程内缓存**（已在 v3.0 ADR-013 设计，命中率 95%）：Cell 进程内 LRU + SoA 布局内存缓存，零网络开销
- **L2 本地 KV 存储**（RocksDB / LMDB）：进程本级磁盘缓冲，异步压缩落库；WAL 防丢失
- **L3 冷归档**（对象存储 / MySQL 保留为只读归档）：历史数据压缩存，按需取回
- **事件溯源**：玩家操作 = 事件流 → 追加到 WAL → 批量压缩快照。状态 = 快照 + 重放增量事件。
  无需"UPDATE … WHERE version"（CAS 语义），无需"脏数据集合"，无需 30s 批量回读。

**迁移策略**：保留 MySQL 为冷归档（不删），新增 RocksDB 做 WAL + 本地 KV。数据层代码的 `BackingStore` 抽象层已是接口——写一个 `RocksDBStore` 实现即可切换，零上层改动。

### 2.4 协议层：FlatBuffers+Protobuf 双轨 → Cap'n Proto 统一 + 位压缩

**为什么推翻**：两种序列化 = 两套构建链 + 两套内存模型。FlatBuffers 零拷贝但体积大；
Protobuf 紧凑但需序列化。Cap'n Proto 统一两者优势：**零拷贝 + 紧凑 + 无序列化代码**。

**目标方案**：
- 所有协议（高频 + 配置）统一用 Cap'n Proto schema
- 高频消息叠加**位压缩**轨道（v2.0 设计）：移动→16bit 定点数，技能→字节码，浮点→半精度
- 构建链简化：一个 `capnp` 编译器替代 `flatc + protoc`，CI 依赖减半
- **带宽预估**：单玩家高频消息从 ~2KB/s → ~500B/s（Cap'n 紧凑 + 位压缩 + AOI 差分）

**迁移策略**：**优先推进**——这是唯一不碰运行时就能做的重构。从 schema 文件开始重写，生成 C++ 代码，逐步替换 FlatBuffers/Protobuf 引用。Cap'n Proto 社区更活跃（Cloudflare Workers 底层协议），生态比 FlatBuffers 好。

### 2.5 内存体系：散落 malloc → Arena 分配器 + 无锁数据结构

**为什么推翻**：`std::string`/`std::vector` 热路径分配 + 多模块独立对象池 = 碎片 + cache miss。
游戏服务器的内存生命周期极其规律（请求→处理→释放），天然适合 Arena。

**目标方案**：
- **请求级 Arena**：每个请求处理周期分配一个 Arena（如 4KB 页），所有临时对象（string/vector/消息）走 Arena，请求处理完整个 Arena 一次性 free（O(1) 释放）。热路径零 malloc。
- **无锁 SPMC 队列**：替代 `std::mutex + std::queue` 的消息传递。单生产者多消费者环形队列，原子操作无锁。
- **无锁哈希表**：玩家在线表、路由表等高频查找用 `folly::ConcurrentHashMap` 或自定义开放寻址 + 原子。

**迁移策略**：从 `common/arena.h` 开始（Arena 分配器是基础设施，放 common 层），逐步替换项目中的 string/vector 临时分配。SPMC 队列用于 EventBus 重构（取代目前设计文档里的进程内 EventBus）。

---

## 3. 实施路线图（三阶段 × 4 周 = 12 周）

### 阶段 A：基础设施重建（第 1-4 周）——**这四周决定未来几年的效率**

| 周 | 任务 | 产出 |
|----|------|------|
| W1 | **Cap'n Proto 统一协议 schema** + 替代所有 .fbs/.proto 生成 | 单构建链，CI 依赖从 2 减为 1 |
| W2 | **Arena 分配器** (`common/arena.h`) + SPMC 队列 (`common/spmc_queue.h`) | 公共基础设施，无外部依赖 |
| W3 | **ECS 核心框架** (`game/ecs/`：EntityManager/ComponentStore/System) | SoA 布局，数据导向替换 OOP |
| W4 | Cap'n Proto 替换 codec 层 + 带宽基准测试 | 验证：编码体积 ↓30-50%，解码吞吐 ↑ |

### 阶段 B：运行时重构（第 5-8 周）

| 周 | 任务 | 产出 |
|----|------|------|
| W5 | **Cell 框架** (`game/cell/`：Cell/CellManager/CellLoader) | 100m×100m 进程隔离里程碑 |
| W6 | **共享内存 IPC** + RingBuffer | 同机 Cell 间 0.02ms 通信 |
| W7 | C++20 协程化 Gateway | 单线程处理万级连接（无上下文切换） |
| W8 | L1 进程内缓存落地 | 命中率 95%+，Redis QPS 降 90% |

### 阶段 C：数据层替换（第 9-12 周）

| 周 | 任务 | 产出 |
|----|------|------|
| W9 | **RocksDB 嵌入落库** (`RocksDBStore` 实现 BackingStore) | 本地 WAL + 压缩快照 |
| W10 | 事件溯源框架 + 流式快照 | 替代 30s 批量落库 |
| W11 | 全量集成回归 + 5 万在线模拟压测 | 端到端验证 |
| W12 | 迁移文档 + 灰度开关 + 旧代码清理 | 平滑迁移，MySQL 保留为只读归档 |

---

## 4. 收益预估（vs 现状）

| 维度 | 现状 | 目标 | 提升 |
|------|------|------|------|
| **内存** | 每 Cell 进程 ~150MB（含数据） | Arena + SoA ~50MB | **↓ 67%** |
| **CPU** | 线程池上下文切换 + 锁 | 协程 + 无锁 + SIMD | **↓ 50%** |
| **带宽** | FlatBuffers ~2KB/s/玩家 | Cap'n + 位压缩 ~500B/s | **↓ 75%** |
| **延迟** | Gateway→GameNode gRPC 1ms | SHM IPC 0.02ms | **↓ 98%** |
| **数据读取延迟** | Redis ~3ms | L1 进程内 0.01ms | **↓ 99.7%** |
| **启动成本** | 30 容器 ~12GB（Dev-Std） | 3 进程 ~1GB（Cell 起步） | **↓ 92%** |
| **扩展粒度** | 50 GameNode 手动 | Cell 级自动调度（100m² 进程） | 质变 |
| **故障隔离** | 单 GameNode 崩溃影响 1000 人 | 单 Cell 崩溃影响几十人 | 100× 缩小 |

---

## 5. 与已交付资产的关系（保留 vs 重做）

| 资产 | 保留 | 理由 |
|------|------|------|
| 21 配置 proto 定义 | ✅ **保留 schema**，换成 .capnp 格式重新生成 | 数据不变，格式重写 |
| Gateway 层代码 | ⚠️ **替换 codec 层**（Cap'n Proto），保留 connection/FSM/限流/路由 | 运行时逻辑正确，重做的是序列化 |
| 数据层 BackingStore/CacheProxy/VersionedStore | ✅ **保留接口**，替换实现（RocksDBStore） | 抽象层设计合理 |
| 数据层 MySQL 相关 | ⚠️ **降级为只读归档**，引入 RocksDB 做主力 | 不删，平滑切换 |
| Docker/CI/构建体系 | ✅ **保留，适配新依赖**（capnp 替代 flatc+protoc） | 基础设施保留 |
| Scale-to-Fit / Content Modularization | ✅ **保留**，与目标架构正交 | 配置驱动理念正确 |
| Kafka DLQ / gRPC DataService | ⚠️ **降级**：事件溯源下 Kafka 只用做异步日志，gRPC DataService 被 RocksDB 本地 I/O 替代 | 保留为可选扩展 |

---

## 6. 关键决策点（需你在下一步确认）

1. **是否从 Cap'n Proto 统一协议开始**（阶段 A 第 1 周）——收益：构建链减半、带宽 ↓30-50%、前向兼容。不碰运行时代码，风险最低、收益最大。**推荐优先启动**。

2. **ECS 框架是自建还是引库**（Entt？）——自建更轻量（无 Boost 依赖），但 Entt 是社区最成熟的 C++ ECS 库（header-only，MIT 协议）。**推荐 Entt**——省实现时间，业界验证。

3. **RocksDB 替代 MySQL**——是否接受"去关系型"路线？RocksDB 是 Facebook 开源的嵌入式 KV（LSM-tree），内存占用可控（可配 block cache），不做 SQL 解析。**如需 SQL 查询能力可保留 MySQL 读库**（不冲突）。

4. **是否立即启动阶段 A**——基础设施重建（Arena + Cap'n + ECS + SPMC 队列）这四个组件可以**独立开发、独立测试**，不依赖现有代码修改，可在四周内交付并跑通基准测试。

---

> 附：本白皮书与 v3.0 架构文档（`docs/architecture/architecture-spec.md` ADR-011~016）的关系——本白皮书是 v3.0 的**落地执行计划** + v3.0 未覆盖的领域补充（无锁数据结构、协议统一、Arena 分配器）。
