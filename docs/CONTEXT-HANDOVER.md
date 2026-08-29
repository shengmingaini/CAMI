# CAMI 上下文交接文档

> **用途**：本文件是历史对话清除后的**唯一权威上下文来源**。新会话只需读这一个文件，即可接上工作。
> **生成日期**：2026-08-29
> **生成依据**：`.workbuddy/memory/` 下 7 份日志（2026-08-06 ~ 2026-08-14）+ 旧 `MEMORY.md` 交叉验证 + 仓库 `git log` 实测核对（47 commit）。
> **冲突消解规则**：日志前后矛盾处以**时间更晚的**为准，标注 `⚠️前后有变化`。
> **标记 `🔴`** = 旧 MEMORY.md 未收录、最容易丢失的信息。

---

## 0. 仓库现状快照（2026-08-29 实测核对）

| 项 | 值 |
|---|---|
| 远程 | `git@github.com:shengmingaini/CAMI.git`（**private**，SCP 形式，走 `github.com:22`） |
| 本地 `dev` | `34f0fd3` — **领先 `origin/dev` 1 笔，未推送** |
| 远端 `origin/dev` | `f13f973` |
| 本地 `main` | `ba8ec30`（停在 2026-08-10，落后 dev 约 37 commit）；`origin/main` 已不存在 |
| `feat-gap7` | `995d35b`，远端已删，**废弃分支待清理** |
| 工作树 | 干净，仅残留 `?? NUL`（Windows 保留设备名，0 字节，提交不了，无害） |
| 提交总数 | 47 |
| 测试基线 | OFF 构建 **17/17 ctest 绿**；本地 MODULES=ON **18/18 全绿**（`34f0fd3`） |
| 网络 | `github.com:22` ✅ 通；`github.com:443` ❌ 墙；`ssh.github.com` 22/443 **均被墙** |

⚠️ **`main` 严重落后**：Week2/3/4 + 阶段 A 全部只存在于 `dev`。任何以 `main` 为起点的操作都会拿到极旧代码。

---

## 1. 项目时间线

> 日志里的「Day N / Week N」是**计划任务卡的名义日期**，与实际执行日期严重脱节。下表左侧为**实际执行日期**。

### 2026-08-06 — 立项 + 架构设计
- 确认 11 条架构假设；产出 `docs/architecture/architecture-spec.md`（13 章 + 4 ADR，v1.0）
- WoW 对标 `docs/architecture/cami-vs-wow-analysis.md`：超越 7 项 / 不及 10 项，发现 3 个 P0 遗漏（客户端预测纠偏、寻路碰撞、AOI LOD）
- 同日连推两个大版本：
  - **v2.0**（`optimization-v2.md`）：补 P0 三项 + P1 三项（Phasing/法术批次/位压缩），ADR-005~010
  - **v3.0**（`optimization-v3.md`）：6 大瓶颈优化，ADR-011~016，共 16 条 ADR
- 建 Docker 环境：`docker/docker-compose.yml` + MySQL 11 表 + Redis + Prometheus
- Day2 技术验证 Demo：`benchmark/tcp_echo_server.cpp` / `tcp_echo_benchmark.cpp`

### 2026-08-07 — Day2 验证 + Day3 协议与设计
- Day2 三项验证全 PASS：**TCP Echo 388,651 QPS** 🔴 / Redis Cluster 3主3从 8/8 / MySQL+ShardingSphere 15/15
- Day3 R1 协议基础：`docs/protocols/protocol-spec.md` + 6 `.fbs` + 5 `.proto`
- Day3 R2 核心 4 模块设计：`docs/modules/{character,combat,aoi,scene}.md`
- Day3 R3 配置表 schema：`config_items/skills/quests.proto`
- Day3 R4 体验闭环三模块：`docs/modules/{quest,economy,social}.md`
- 协议验收发现 4 缺口（G1 命名 / G2 版本字段 / G3 清单文件 / G4 目录不完整）→ 方案 B 补齐 `quest/economy/social.fbs` + `protocol-manifest-v1.md`

### 2026-08-10 — 项目密度最高的一天（一次会话完成约 3 周名义任务量）
- **机制层 21 个 config proto 全部交付**（config_*：vendors/currencies/buffs/professions/宝石/附魔/套装/zones/世界事件/spawns/pvp…），`systems-catalog.md` §4 待建清零
- **🔴 协议编译校验（重大转折）**：
  - 装 protoc（受管 venv `C:/Users/17283/.workbuddy/binaries/python/envs/default` + `pip install grpcio-tools`），22 个 `.proto` 逐批复验 exit=0 零 warning
  - 装 flatc v25.12.19 → `F:\AI\flatc\flatc.exe`，9 个 `.fbs` 编译**抓出 1 处真 bug**：`aoi.fbs` 的 `class_id` 默认 0 不在 `ClassId` 枚举内（原从 `Warrior=1` 起无 0 成员）→ `common.fbs` 补 `None = 0`
  - ⚠️前后有变化：08-07 记「环境无 flatc/protoc，schema 仅人工校对（GAP-3）」→ **08-10 已消除**
- **内容填充 + 平衡校验工程化**：`data/configs/*.json` 13 个 ConfigSet + `verify/balance_verifier.py`（10 大类校验 A–M 段）+ `verify/load_configs.py`
- **Day4 工程基建**：根 CMake 五层 `add_subdirectory` 接入、五层 STATIC target、`.github/workflows/ci.yml`
- **Day5 事件总线 + 编码规范**：`common/event_bus/mpmc_queue.h`（Vyukov MPMC）+ `event_bus.h` + `benchmark/event_bus_bench.cpp` + `docs/architecture/coding-standards-v1.md`
  - 实测 🔴：MPMC 裸队列 **18.33 / 13.88 Mmsg/s**（1P1C/4P4C）；EventBus 端到端 **19.54 / 14.77 Mmsg/s**，远超 ≥100 万 msg/s 验收线
- **经济 GAP-1~9 全部闭合**：最终 **PASS=59 WARN=0 FAIL=0**
- **git 化 + CI 双防线**：`git init -b main`（此前**不是 git 仓库**），初始提交 `6cddb26`（189 文件）；加 `economy-balance` job（`6eb06ac`）
- **CI #1 → #4 排障链**（详见 §4.6）：最终 `ba8ec30` 双绿
- **Week2 网关 D1-D4**：connection（`5075d87`）→ codec → heartbeat（`995d35b`）→ 单测补齐 + 带宽基准
  - 带宽实测 🔴：encode **104.40 MB/s**、decode **2063.38 MB/s**，30 万帧往返一致

### 2026-08-11 — Week2 收尾 + Week3 全周 + Week4 D1-D5 + WSL 排障
- Week2 D5 压测 harness：`benchmark/stress/` + `scripts/tuning/tune-50k-connections.sh`
- **工作流定型** ⚠️前后有变化：08-10 走 `feat-gap7` 特性分支 → **08-11 用户拍板「以 dev 往下推进」**，废弃特性分支循环
- **Week3 网关 5 模块**：security `4cdbeb3` / ratelimit `46835b8` / router `9a9dbab` / redis `5466c4e` / 迁移设计 `2d50287`
- **Week3 风险闭环**：push 恢复、行尾归一化 `94aba22`、集成缝 `GatewayPipeline` `0597a36`、周报 `3d1a581`
- **真实后端 CI job**：`build-modules-on` `bee5e06` + 修 YAML `f28fa40` / 镜像 `64c7fac` / 依赖 `203010e` / redis++ 包名 `af0a570`
- **Week4 数据层**：D1 schema → D2 SS 8 分库 + 读写分离（实库验证）→ D3 `cache_proxy` → D4 `sync_manager` → D5 `version` + TPS → D6 生产后端
- **WSL2 排障长链**（23:00 → 次日 01:00）：最终根因 = `.wslconfig` 的 `networkingMode=mirrored`

### 2026-08-12 — D6 方案B + CI OOM 治理 + Scale-to-Fit + 内容模块化 + 热路径优化 + 阶段A 启动
- D6 五 commit `f4a8a35`/`fc811bd`/`898ac18`/`bd75f3e`/`ee7eeae`（**一度全部未提交，风险极高**）
- **🔴 P0 schema 契约错配发现**：`player_base` 无 `payload` 列，而 `mysql_backing_store` 落库 SQL 引用它 → 运行时 `Unknown column`；且 `REPLACE INTO` 会清空 `account_id`（NOT NULL 无默认 → INSERT 直接失败）
- **方案 B 落地** `b08affd`：新建 `player_state(player_id,payload,version)` 分片表 + 多副本 CAS 下沉 MySQL + gRPC Health
- CI OOM 治理：`58be49c`（限并发）+ `9dda34b`（release-only）+ `docs/ops/ci-optimization.md`
- **Scale-to-Fit 最小起步**：`docs/architecture/scale-to-fit.md` + `docker/docker-compose.mini.yml`（≈1.5GB vs Dev-Std 30 容器 ≈12GB）
- **内容模块化 P0**：`game/content/`（ContentRegistry 16 模块 + ConfigManager + LuaModuleLoader），16 用例绿，OFF 13/13
- **网关/数据层热路径优化**：Connection 内嵌 FrameDecoder（零拷贝帧视图）、心跳下沉 per-connection timer、stats 改 atomic、dirty 存 {key,value}
- **🔴 彻底优化白皮书**（`docs/architecture/radical-optimization-blueprint.md`）：**推翻 v1.0 传统脚手架**，三阶段路线 A/B/C，每阶段 4 周。用户确认：Cap'n Proto 统一协议 / Entt ECS / RocksDB 主力 + MySQL 归档 / 立即启动阶段 A

### 2026-08-13 — 阶段 A W1-W5 + CI 报错修复链（6 连击）
- **W1** Cap'n Proto：`proto/capnp/` 10 个 schema（`$Cxx.namespace("cami")`）+ `scripts/capnp_lint.py` 结构护栏
- **W2** Arena + SPMC：`common/arena.h` + `common/spmc_queue.h`，9 用例绿
- **W3** ECS SoA：`game/ecs/components.h`（9 POD 组件 + static_assert 锁布局）+ `ecs_world.h`（Entt 门控）
- **W4** RocksDB：`data/rocksdb_proxy/rocksdb_backing_store.{h,cpp}`（三列族 state/meta/events + 事件溯源骨架）+ `storage_router.h`（Shadow→ActivePrimary→FallbackOnly 三态机）
- **W5** QUIC 骨架：`gateway/transport/transport.h`（ITransport 抽象）+ `in_memory_transport.h` + `connection_migrator.h`（同构三态机）+ `quic_transport.{h,cpp}`（msquic 门控）
- **P0-1 MODULES=ON 静态预检**：修 10 处（3 处确定编译错误 + 4 处 CMake target 名错误 + 3 处健壮性）
- **CI 六连修复**：`f48d61d` → `ebcc350`（浅克隆 baseline）→ `291c39f`（overlay triplet）→ `5e3c5b1`（host triplet）→ `63270a3`（libmariadb）→ `3fbf8b4`（vcpkg boost）→ `245f1f0`（redis++ API）→ `f13f973`（cppkafka API）

### 2026-08-14 — 🔴 CI 计费熔断 + 本地 MODULES=ON 跑通（里程碑）
- **🔴 GitHub 账户计费失败**：所有 job 8 秒未启动即红，原文 `The job was not started because recent account payments have failed or your spending limit needs to be increased.` **与代码完全无关**。诱因：私有仓库 Actions 免费额度（约 2000 分钟/月）被反复全量 vcpkg 构建（首次 ~1.5h）耗尽
- **🔴 本地 MODULES=ON 完整跑通**：本地 vcpkg `C:/vcpkg` 装 entt/capnproto/rocksdb（12min），classic mode configure，**18/18 ctest 全绿**。真编译暴露并修 5 处「纸面绿」问题，commit `34f0fd3`（**未推送**）

---

## 2. 架构决策清单

### 2.1 顶层架构（ADR-001~016，`docs/architecture/architecture-spec.md` v3.0.0）

| ADR | 决策 | 为什么 | 否掉的备选 |
|---|---|---|---|
| 001 | 进程内 EventBus + Kafka 异步 + gRPC 跨服务 + Redis Pub/Sub 预留 | 分层解耦，热路径零 IPC | 全 gRPC（延迟 1ms 不可接受） |
| 002 | **WoW 模式**：character 独占 Player 内存实例，他模块 `const Player&` 读 + `ApplyXxx()` 写 | 单一写入口，避免隐式并发写 | 各模块直写字段 / 共享可变引用 |
| 003 | PlayerID hash 分片（Redis 16 / MySQL 8）+ 拍卖行/公会独立库 | 分片键统一，跨服预留 | 按账号分片（换角色跨库） |
| 004 | vcpkg + Sol2 + WSL2 编译 / Docker 服务 | 依赖可复现 | 系统包管理器（跨平台不一致） |
| 005-010 | 客户端预测纠偏 / NavMesh+LOS 缓存 / AOI LOD 三级 / 法术批次(PVP 100-200ms) / 双轨协议 / Phasing | 补 WoW 对标 3 个 P0 + 3 个 P1 差距 | 纯服务器权威（移动体验差）、全量广播 |
| 011 | 共享内存 IPC（SHM + SPSC RingBuffer）替代同节点 gRPC，延迟 1ms→0.02ms | 同节点 IPC 是最大瓶颈 | 全 gRPC / Unix socket |
| 012 | 场景级线程隔离 + Job System + ECS SoA + MPMC 无锁队列 | CPU 12.5%→75% | 全局线程池 + 细粒度锁 |
| 013 | 三级缓存（L1 进程内 95% → L2 Redis 直连 → L3 MySQL）+ WAL | 热数据读取 -99.7% | 单层 Redis |
| 014 | QUIC 0-RTT + 边缘网关 | 重连 ~0ms、RTT -80% | 纯 TCP |
| 015 | 4 级背压（GREEN/YELLOW/ORANGE/RED）渐进降级 | 硬性 85% 阈值会雪崩 | 硬性阈值熔断 |
| 016 | Cell-based 开放世界（100m Cell / 25 线程 / 每线程 40 人） | 负载均衡粒度可控 | 整场景单线程 |

### 2.2 协议层

| 决策 | 内容 | 为什么 | 否掉的备选 |
|---|---|---|---|
| 双轨协议 | 轨道 A 高频 UDP/QUIC 位压缩（≤15B/包，6-bit opcode）；轨道 B 可靠 QUIC 流 + FlatBuffers | 带宽 1365KB/s→47KB/s（-97%） | 全 FlatBuffers / 全 JSON |
| 类型判别 | 轨道 B 用 FlatBuffers union `CAMI.MessageBody`，**无独立数值 msg_id** | 避免双源真相 | 十六进制 MsgID 表 |
| 加密 | AES-128-GCM + X25519/HKDF 握手；A 轨 tag 截断 8B / B 轨 16B + 滑动窗口防重放 | 安全性与带宽平衡 | 全 16B tag（浪费 8B/包） |
| 文件组织 | 按域拆分 9 `.fbs` + 22 `.proto`，`protocol-manifest-v1.md` 作统一索引 | 避免巨型单文件 | 任务卡字面的单文件 `protocol.fbs` |
| **⚠️前后有变化** | 2026-08-13 起**新增 Cap'n Proto 作为统一协议方向**（`proto/capnp/` 10 schema），FlatBuffers/Protobuf 保留为旧层 | 零拷贝 + 无解析步骤，省 CPU 与内存 | 继续深化 FlatBuffers |
| 跨集引用 | config proto 之间跨集引用统一用裸 `uint32` ID，不用 import | 防循环依赖 | 互相 import |

### 2.3 数据层

| 决策 | 内容 | 为什么 | 否掉的备选 |
|---|---|---|---|
| 账号/角色分离 | account 落全局库 `cami_global`（无法按 player_id 路由）；新增 `account_character`（登录列举，避免广播 8 分库）+ `player_name_reservation`（角色名跨分片唯一） | 分片键与查询模式匹配 | account 也按 player_id 分片（登录无法路由） |
| n-cap 强制 | 单账号 n=5（可扩 8-10）；Data Service 用**原子条件更新** `UPDATE account SET character_count=character_count+1 WHERE account_id=? AND character_count < n`，affected_rows=0 即拒 | InnoDB 行锁原子，免乐观锁版本 | 应用层先读后写（并发超额） |
| 反范式白名单 | 核心玩家表严格 3NF；仅 `player_mail.sender_name` 与 `auction_listing.seller_name` 故意反范式 | 明确边界，防反范式蔓延 | 全 3NF（跨分片 JOIN 不可行） |
| 货币存储 | `amount` BIGINT 整数最小单位，**禁浮点** | 浮点累加误差 | DECIMAL / DOUBLE |
| 分片铁律 | **逻辑 8 分片固定（`player_id % 8`），物理实例 1→8 渐进** | 扩容 = 起新实例 + 搬库 + 改 SS URL，算法与代码零改动、数据零重分布 | **%1 起步**（取模扩分片需全量重分布，明确否决） |
| **⚠️前后有变化** | 分库数：08-06 起 compose 为 2 分片（`%2`）→ 08-11 W4-D2 升 8 分库 + 8主8从读写分离 → 08-12 Scale-to-Fit 改为**逻辑 8 分片但对单物理实例**（Dev-Mini ≈1.5GB） | 起步负载几十人，不该按 5 万在线铺满 | 一开始就 30 容器 ≈12GB |
| MySQL 客户端 | **libmariadb**（MariaDB Connector/C 3.4.x）替代 vcpkg `libmysql` | libmysql 实为 MySQL 官方 8.0.46，依赖 boost×5+ncurses+lz4+zstd+rapidjson 且硬编码 boost_1_77_0，构建反复失败；libmariadb 是 `<mysql.h>` drop-in 替代，依赖仅 zlib+openssl | vcpkg libmysql（CI 第五次红的原因） |
| 多副本 CAS | **方案 B（读写路径 DB 版本权威）** | 方案 A 仅落库端 CAS，`VersionedStore` 进程内存态跨副本失效 | 方案 A（仅落库端） |
| 存储主力 | RocksDB 主力 + MySQL 归档（阶段 A W4） | 消灭 Dev-Std 的 MySQL+SS+Redis 外部栈，数据层外部依赖 ≈12GB → 嵌入式单进程 | 继续 MySQL + SS + Redis |

### 2.4 网关层

| 决策 | 内容 | 为什么 | 否掉的备选 |
|---|---|---|---|
| 传输抽象 | `gateway/transport/transport.h` ITransport（16B 不透明 ConnectionId）+ InMemoryTransport（进程内仿真）+ ConnectionMigrator（三态机）+ QuicTransport（msquic 门控） | TCP 作基线、QUIC 作候选**同构治理**，可影子评估后自主晋升 | 直接替换 TCP 为 QUIC（无回退） |
| 🔴 三态机治理哲学 | Shadow（影子双写/影子迁移，静默评估）→ ActivePrimary（自主晋升）→ FallbackOnly（熔断回退基线 + 告警）。**硬闸门 = 正确性 + 健康；延迟仅软门槛** | 杜绝失控环路；存储迁移与网络迁移共用同构模型 | 一次性切换 / 人工开关 |
| 心跳权威 | 管理器级、纯 std、时钟注入式 `HeartbeatManager`；tick **锁内收集过期条目、锁外调 kick** 防重入死锁 | 可确定性单测（注入假时钟） | 绑 Asio timer 到 Connection（不可测） |
| **⚠️前后有变化** | 08-12 热路径优化：心跳**下沉为 per-connection idle_timer**（`mark_activity` 只 `expires_after`）；Connection **内嵌** FrameDecoder（值成员 + `set_on_frame` 帧视图零拷贝） | 消除 2 全局锁 + 每帧 vector 堆分配 + 每消息 timer cancel/重建 | 原集中式管理器 |
| 帧视图约束 | `on_frame` 回调内**不得重入 consume**；跨回调持有需显式拷贝 | 零拷贝的代价 | 拷贝语义（多一次 alloc） |
| 一致性哈希 | FNV-1a 64-bit **+ fmix64 终段混淆**（MurmurHash3 终段） | 纯 FNV-1a 短串雪崩不足，虚拟节点环上聚类 → 10 节点时某节点占 19%（1.9×均值） | 纯 FNV-1a |
| 故障切换重路由 | primary 下线后用**二次独立哈希** `hash_fn(h)` 分散 | 固定黄金比例步长线性探测会让原落点 key 全堆到单一分片（max 2444/1250 ≈ 2×） | 固定步长线性探测 |
| 多 acceptor | Linux `SO_REUSEPORT` 多 acceptor；Windows **强制单 acceptor**（`#ifdef __linux__` 守卫） | Windows `SO_REUSEADDR` 不允许同端口多 bind（失败 10048） | Windows 也多 acceptor（压测暴露的 bug） |
| 集成缝 | 四缝（ratelimit/security/router/redis）组合成 `GatewayPipeline`，**仅经 `ConnectionManager::set_on_accept` 挂钩接入，不改其本体** | Connection 不携带 player_id，强塞钩子会扭曲设计 | 在 Connection 上加 player_id 钩子 |

### 2.5 编码规范（`docs/architecture/coding-standards-v1.md`）
- 命名 / 包含（IWYU，**仓库根全路径**）/ 错误处理（可预期失败用返回值，异常仅真异常）
- 并发：热路径无锁优先、内存序 acquire/release、伪共享 `alignas(64)`、冷路径可加锁
- 资源：RAII 禁裸 `new`；CMake：**target 级**、WIN32 守卫 Winsock
- 测试：`selfcheck`（无 GTest/无 socket，接 CI）+ ctest 双层；格式化 2 空格 100 列
- **层依赖数组铁律**：永不用零长度裸数组，统一 `std::initializer_list<const char*>`

---

## 3. 已交付成果清单

### 3.1 五层 monorepo 骨架
| 层 | target | 关键文件 | 依赖 |
|---|---|---|---|
| common | `cami_common` | `common/arena.h`、`common/spmc_queue.h`、`common/event_bus/{mpmc_queue.h,event_bus.h}`、`common_layer.{h,cpp}` | — |
| data | `cami_data` | `data/redis_proxy/`、`data/sync/`、`data/version/`、`data/mysql_proxy/`、`data/rocksdb_proxy/`、`data/data_service/`、`data/configs/` | → common |
| game | `cami_game` | `game/ecs/`、`game/content/`、`game_layer.{h,cpp}` | → common + data |
| gateway | `cami_gateway` | 11 个子模块（见 3.3） | → common |
| ops | `cami_ops` | `ops_layer.{h,cpp}` | → common |

单向依赖 `gateway→game→data→common`、`ops→common`。`cami_skeleton_check` **无条件**执行（`enable_testing()` 与 `add_test` 不受 `CAMI_BUILD_TESTS=OFF` 影响）。

### 3.2 common 层
- `common/event_bus/mpmc_queue.h`：Vyukov 有界 MPMC 无锁队列（cell `alignas(64)`、容量须 2 的幂否则 `std::terminate()`、acquire/release 内存序）
- `common/event_bus/event_bus.h`：`Channel<T>`（publish 仅入队无锁 / drain 取一批调订阅者快照无锁 / subscribe 冷路径加锁）+ `EventBus<T>`（同类型多具名通道）
- 实测：MPMC 18.33/13.88 Mmsg/s，EventBus 端到端 19.54/14.77 Mmsg/s（1P1C/4P4C，GCC 16.1.0 Release）
- `common/arena.h`（W2）：请求级 Arena（4KB 页 × 游标 + 8B 对齐，O(1) Reset）
- `common/spmc_queue.h`：无锁 SPMC 环形队列（CAS 竞争 + cache line pad）

### 3.3 gateway 层（11 模块）
| 模块 | 路径 | 依赖 | 单测 |
|---|---|---|---|
| connection | `gateway/connection/` | Boost.Asio | `connection_test.cpp`（FSM 7 态 + 池） |
| codec | `gateway/codec/` | 纯 std | `codec_framing_test.cpp`（粘包/半包/超大包/精确边界） |
| heartbeat | `gateway/heartbeat/` | 纯 std | `heartbeat_test.cpp`（误差<1s / 无泄漏） |
| security | `gateway/security/` | OpenSSL（MODULES 门控） | `security_test.cpp`（5 例） |
| ratelimit | `gateway/ratelimit/` | 纯 std | `ratelimit_test.cpp`（5 例） |
| router | `gateway/router/` | 纯 std | `router_test.cpp`（5 例） |
| redis | `gateway/redis/` | redis++（MODULES 门控） | `redis_test.cpp`（3 例 + RealBackendRoundTrip） |
| integration | `gateway/integration/` | 四模块组合 | `gateway_pipeline_test.cpp`（6 例） |
| transport | `gateway/transport/` | msquic（MODULES 门控） | `connection_migrator_test.cpp`（6 例） |

⚠️ `gateway/migration/` 是空目录（历史遗留，待清理）。

### 3.4 data 层
| 模块 | 路径 | 实测数字 |
|---|---|---|
| 缓存代理 | `data/redis_proxy/cache_proxy.{h,cpp}` | Zipf(s=0.9) 100 万次读：**命中率 100.00%**（≥95% PASS）、**2,980,239 ops/s**；写回 1000 dirty 全落库；热点识别 1956 个 |
| 数据同步 | `data/sync/sync_manager.h` | 周期落库 flush_count=100；批量落库 20 万条 **757,927 条/s** |
| 版本校验 | `data/version/version.h` | 8 线程并发 Cas(expected=0) **仅 1 成功**（7 个 100% 拦截）；乐观自增 8×2000 轮=16000，最终值=期望，无丢失 |
| MySQL 后端 | `data/mysql_proxy/mysql_backing_store.{h,cpp}` | 单分库 **9784 TPS**（≥5000 PASS） |
| Kafka | `data/sync/kafka_flush.cpp` | 异步落库 + DLQ + at-least-once（**未实跑**） |
| RocksDB | `data/rocksdb_proxy/` | 三列族 + 事件溯源骨架；5 用例绿（**未跑真实 DB**） |
| Data Service | `data/data_service/` | gRPC 封装，GameNode 禁止直连 DB（**从未真编译**） |

### 3.5 game 层
- `game/ecs/components.h`：9 个 POD 组件 + 编译期 `static_assert` 锁 size/对齐
- `game/ecs/ecs_world.h`：Entt 封装（MODULES 门控）
- `game/content/`：`content_types.h`（Phase 枚举 + ContentModule + ModuleState）、`content_registry.{h,cpp}`（内置 **16 模块表**；EnsureLoaded 依赖拓扑 DFS + 幂等 + 循环依赖检测；IsUnlocked 双闸门；LevelCap **30/45/60/70** 四段 + SetLevelCap 扩展接口）、`config_manager.{h,cpp}`（LRU + 64MB 上限 + ConfigSource 抽象）、`lua_module_loader.{h,cpp}`（懒 require + Invalidate 热更）
- `game/{navigation,collision,prediction,character,combat,aoi,scene,quest,economy,social}/`：**仅目录，无代码**

### 3.6 协议与配置
- `proto/flatbuffers/`：9 个 `.fbs`，flatc v25.12.19 **真编译通过**
- `proto/protobuf/`：22 个 `.proto`，protoc **真编译通过零 warning**
- `proto/capnp/`：10 个 schema（W1），**无 capnp 编译器，仅 `scripts/capnp_lint.py` 结构护栏**
- `proto/data_service.proto`（gRPC 服务定义）
- `data/configs/`：13 个 ConfigSet JSON
- `verify/_pb/`：22 个 `_pb2.py` 桩**已提交入库**（CI 不依赖运行时 protoc）

### 3.7 验证与运维资产
- `verify/balance_verifier.py`：A–M 段 10 大类校验（引用完整性 / 经济源汇对账 / 币种周上限 / 刷新密度 / 世界事件币 / PvP / 专业净注入 / 区域等级带 / 门可达性 BFS / 结构单调 / XP / 战斗 / 修理 / 囤积）。**最终 PASS=59 WARN=0 FAIL=0，exit 0**
- `verify/combat_simulator.py`：物理/法术双路径，3 职业，离散度比 **1.153**（阈值 1.5）
- `benchmark/`：tcp_echo、event_bus_bench、gateway_bandwidth_bench、security_bench、`stress/gateway_stress_{server,client}.cpp`
- `docker/`：compose（Dev-Std 30 容器）+ compose.mini（3 中间件）+ MySQL init/verify/repl 脚本 + SS 配置 + Prometheus
- `scripts/`：tuning、benchmark、chaos、capnp_lint.py、verify_*.sh、build_msys2.sh

### 3.8 测试总账
| 构建模式 | ctest | 状态 |
|---|---|---|
| OFF（Boost only） | **17/17** | 本地 MinGW 绿（08-13 build-w5） |
| MODULES=ON | **18/18** | 本地 MinGW 绿（08-14 build-mod-local，`34f0fd3`） |

18 = 根 CMake 11（skeleton/codec/heartbeat/connection/security/ratelimit/router/redis/integration/content/arena_spmc）+ game/ecs 2（ecs_component / ecs_world，后者仅 ON）+ data 4（cache_proxy_demo / version_demo / sync_demo / storage_router）+ gateway/transport 1（connection_migrator）。

### 3.9 文档清单（68 个 .md）
- `docs/architecture/`（11）：architecture-spec(v3.0.0)、cami-vs-wow-analysis、optimization-v2/v3、coding-standards-v1、event-bus-design、radical-optimization-blueprint、phase-a-progress、scale-to-fit、modules-on-preflight
- `docs/modules/`（17）：_TEMPLATE + 16 模块文档
- `docs/database/`（13）：er-player-sharding(+svg)、account-character-cap、sharding-verification、cache-proxy、sync-verification、version-verification、shard-tps-bench、data-service、cluster-capacity-baseline、d6-delivery-status、d6-code-review、d6-followup-design、week4-review
- `docs/game-design/`（9）、`docs/protocols/`（5）、`docs/ops/`（3）、`docs/weekly/`（3）、`docs/benchmark/`（4）、`docs/verification/`（2）、`docs/design/`（1）

---

## 4. 踩坑与解决方案（最高价值部分）

### 4.1 vcpkg（最高频、最耗时）

| # | 现象 | 根因 | 解法 |
|---|---|---|---|
| V1 | `git clone` vcpkg 全量历史 33 万对象，慢链路下 `early EOF` | 未浅克隆 + 多线程 pack 吃内存 | `--depth 1` + `git config --global pack.threads 1` |
| V2 | ⚠️**与 V1 冲突**：manifest mode 报 `failed to git show aae277ac:versions/baseline.json` (exit 128) | V1 的浅克隆访问不到 baseline commit 的历史 tree | CI：去掉 `--depth 1` 完整 clone（`ebcc350`）；本地：已用 `--classic` 装好依赖后，cmake 加 `-DVCPKG_MANIFEST_MODE=OFF` 强制 classic mode 复用 |
| V3 | 项目根有 `vcpkg.json` 时 `vcpkg install <pkg>` 报 manifest mode 不支持单个包参数 | manifest 模式限制 | 加 `--classic` |
| V4 | Windows 默认 triplet `x64-windows` 需要 Visual Studio（本机无 VS） | vcpkg MSVC ABI 默认 | `--triplet x64-mingw-static --host-triplet x64-mingw-static --overlay-triplets=C:/vcpkg/triplets/community` |
| V5 | ⚠️前后有变化：命令行 `-DVCPKG_TARGET_TRIPLET=...` 无效，仍回退 x64-windows | 用户 MSYS2 终端多行 `\` 续行被吞，后续 flag 整段丢失 | 写进 CMakeLists（`project()` 之前）；**08-14 改用 `-DVCPKG_MANIFEST_MODE=OFF` + 显式命令行 triplet 并用** |
| V6 | vcpkg 工具链路径传 `/c/vcpkg/...` POSIX 形式，原生 mingw64 CMake 不认 | 原生 CMake 把 `/` 解析成当前盘根 → `F:\c\...` | 用 Windows 风格 `C:/vcpkg/scripts/buildsystems/vcpkg.cmake` |
| V7 | vcpkg 工具链屏蔽 MSYS2 Boost（CMP0144 警告忽略 `_ROOT` 变量） | vcpkg 把 find 限在 vcpkg root path | 工具链 include 后 `list(APPEND CMAKE_FIND_ROOT_PATH "C:/msys64/mingw64")` + FORCE `Boost_ROOT`/`BOOST_ROOT` |
| V8 | **vcpkg 不读环境变量 `VCPKG_BUILD_TYPE`**，grpc 仍构建 `x64-linux-dbg` | 它是 triplet 内的 CMake 变量，不是环境变量 | overlay triplet（`set(VCPKG_BUILD_TYPE release)`）+ `vcpkg install --triplet` + `cmake -DVCPKG_TARGET_TRIPLET` **三处对齐** |
| V9 | `.gitignore` 的 `*.cmake` 把 overlay triplet 也忽略了 | glob 规则 | 加 `!cmake/triplets/*.cmake`（`!cmake/` 只取反目录，不取反其中的 .cmake） |
| V10 | `No space left on device`，`vcpkg_installed/x64-linux/` 仍是默认 triplet | **只设了 target triplet，漏了 host triplet**。grpc 有 host 依赖（生成 grpc_cpp_plugin），host triplet 默认固定不跟随 `--triplet` | `--host-triplet x64-linux-release` + `-DVCPKG_HOST_TRIPLET=...`。**release-only 必须 target+host 双 triplet 对齐** |
| V11 | CI 反复 OOM（#23-#26 四次红，~1h22m 中断无错误输出） | 内核 OOM kill：runner 16GB，vcpkg 顶层并发 = nproc 同时编 grpc/protobuf/abseil/c-ares | `VCPKG_MAX_CONCURRENCY: 1` → 仍不够 → release-only → Ninja job pools `compile=2;link=1` 链接串行 |
| V12 | **🔴 `libmysql:x64-linux-release` BUILD_FAILED** | vcpkg 的 libmysql port **实为 MySQL 官方 8.0.46（非 MariaDB！）**，依赖 boost×5+ncurses+lz4+zstd+rapidjson，且硬编码 `boost_1_77_0`（issue #46011） | 换 **libmariadb**（MariaDB Connector/C 3.4.x，`<mysql.h>` API drop-in 兼容，依赖仅 zlib+openssl）。find_package=`unofficial-libmariadb`、target=`unofficial::libmariadb`。⚠️纠正早期「libmysql 底层是 MariaDB」的错误假设 |
| V13 | cmake configure 报 boost_system 版本不匹配（要 1.91.0 找到 1.83.0） | `-DBOOST_ROOT=/usr` 指向 apt boost 1.83.0，与 vcpkg boost 1.91 冲突 | `vcpkg.json` 显式声明 `boost-system` + 移除 `-DBOOST_ROOT=/usr` |
| V14 | vcpkg install 卡在 `Downloading PowerShell-7.6.3-win-x64.zip`（111.5MB，20KB/s 需 1.5h+） | vcpkg 自带构建工具，编译 OpenSSL 需要 | 用 GitHub 加速代理手动下载 zip 放 `%LOCALAPPDATA%\vcpkg\downloads\`，**文件名必须精确**；或有代理时 `export HTTPS_PROXY=...` |
| V15 | 清华 `github-release` 镜像 404 | **该镜像未收录 PowerShell 项目**（只缓存收录列表），不是版本不存在 | 该镜像对 cmake/ninja 有效，对 PowerShell 无效 |
| V16 | MSYS2 官方仓库没有 powershell 包 | packages.msys2.org 查证 "No packages found" | 「pacman 装 pwsh 让 vcpkg 跳过下载」方案**排除** |
| V17 | **🔴 find_package 名 ≠ port 名** | vcpkg port 名 ≠ CMake config 包名 | `redis-plus-plus`→`find_package(redis++ CONFIG)`+`redis++::redis++_static`；`libmysql`→`unofficial::libmysql::libmysql`；`cppkafka`→`CppKafka::cppkafka`；gRPC→`gRPC::grpc++`（**小写**）；`libmariadb`→`unofficial::libmariadb`；`entt`→`EnTT::EnTT`（**大写**）；`rocksdb`→`RocksDB::rocksdb`/`rocksdb`（双名兜底）；`msquic`→`if(TARGET)` 探测。**以 vcpkg 安装输出的 "provides CMake targets" 为准，不要凭 port 名猜** |
| V18 | ⚠️前后有变化：早期判断「libmysql 在 MinGW 永远编不了 → MySQL 后端本地不可验」 | 对 libmysql 成立，但换 **libmariadb 后本地 MinGW 装成（3.4.8）并编过** | 仍不可验的只剩 msquic + grpc |
| V19 | grpc 在 MinGW 极难编（官方主支持 MSVC/Unix） | 源码巨大 + 用户带宽差 | 不建议 MinGW 硬磕 grpc，走 WSL2(x64-linux) 或 CI |
| V20 | `vcpkg install` 首次打印计划后长时间无输出 | 正常首次构建行为 | 看有无 `Building n/N` / `Restoring ... bincache`；**连续 10+ 分钟零输出再怀疑网络** |

### 4.2 CMake

| # | 现象 | 根因 | 解法 |
|---|---|---|---|
| C1 | `zero-size array 'kDependsOn'`，5 层全编译失败 | 空依赖声明为 `const char* kDependsOn[] = {}`，**GCC/Clang 拒绝零长度数组（MSVC 容忍）** | 改 `std::initializer_list<const char*>`；`depends_on()` 由 `kDependsOn[0]` 改 `*kDependsOn.begin()` |
| C2 | CMake 警告 CMP0167（FindBoost 模块在 CMake 3.30+ 移除） | Boost 1.91 应走 CONFIG 包 | `find_package(Boost REQUIRED CONFIG COMPONENTS system)` + `cmake_policy(SET CMP0167 NEW)` |
| C3 | skeleton_layer_check 只引用 constexpr 常量，链接不需 .o，无法验证层是否真接入 | 常量折叠 | 改为调用各层 `layer_name()` 强制链接 |
| C4 | ubuntu 报 `Could not find boost_systemConfig.cmake` | apt `libboost-dev` 元包不含 per-component cmake 配置 | apt 装 `libboost-system-dev`（不只是 `libboost-dev`） |
| C5 | **`CAMI_BUILD_MODULES` 宏未透传给测试可执行文件** → MODULES=ON 下真实路径用例仍被编译剔除（等于没验证） | 宏仅 `target_compile_definitions(PRIVATE ...)` 加在模块自身 | 根 CMakeLists 内加全局 `add_compile_definitions(CAMI_BUILD_MODULES)` |
| C6 | `cami_data` 把 redis++/cppkafka/libmysql 链成 `PRIVATE`，main 直接实例化 → 未定义符号 | 静态库 PRIVATE 依赖不向下传递 | 改 `PUBLIC` |
| C7 | 子目录 `add_test` 不注册 | `enable_testing()` 位置 | 必须在 `add_subdirectory` **之前** |
| C8 | 子目录测试找不到 GTest 目标 | find_package 顺序 | 根 CMakeLists 把 `find_package(GTest/Threads)` **前移到 `add_subdirectory(game)` 之前** |
| C10 | 旧 `build/` 残留 Ninja 缓存拒绝 MinGW 重配置 | 生成器锁 | 用新 `-B` 目录（build-ci / build-mod-local / build-w5 …），别用 rm（safe-delete 会拦截） |
| C11 | RocksDB 11.x 头文件要求 C++20（`operator==()=default` + `using enum`），项目 C++17 | 库要求高于项目标准 | 仅该 TU `set_source_files_properties(... COMPILE_OPTIONS "-std=c++20")`（边界类型标准库，ABI 兼容） |

### 4.3 依赖库版本 / API

| # | 现象 | 根因 | 解法 |
|---|---|---|---|
| L1 | `Boost.Asio 1.91` 的 `expires_after` **仅单参数重载** | API 变更，两参数形式已移除 | 改 `expires_after(std::chrono::milliseconds(ms))`。**静态审查漏掉、实编才抓出** |
| L2 | redis-plus-plus：`mget(first,last)` 2 参 → 新版需 3 参 | API 变更 | 三参 + 输出迭代器 |
| L3 | redis-plus-plus 的 `Optional<T>` 是**自定义类**非 `std::optional` | 库自定义类型 | mget 输出到 `vector<sw::redis::OptionalString>` 后逐元素转 |
| L4 | `pipeline()` 返回 `Pipeline` 不完整类型 | 缺 include | 加 `queued_redis.h` + `pipeline.h` |
| L5 | cppkafka：`header()` 新版单参；`Message` 无 `get_header(name)`；`get_data()` 返回 `unsigned char*` | API 变更 | ① Buffer 需先存左值；② 改 `get_header_list()` 遍历；③ 经 `operator std::string()` 转回 |
| L6 | `cppkafka::PayloadPolicy::BLOCK_ON_FULL_QUEUE` 找不到 | **嵌套枚举** | `cppkafka::Producer::PayloadPolicy::...` |
| L7 | Entt `emplace<聚合>(e, args)` C++17 括号初始化失败（9 处） | C++17 聚合初始化限制 | `emplace(e, T{args})` |
| L8 | **Entt 3.14+ 移除 `registry::alive()`** | API 变更 | 存活实体数 = `reg_.storage<entt::entity>()->free_list()`（const 重载返回指针用 `->`） |
| L9 | RocksDB `ro.iterate_lower_bound = &std::string` 类型不匹配 | 类型 | 局部 `rocksdb::Slice` 变量 |
| L10 | **RocksDB 11.x `DB::Open` 第 5 参是 `unique_ptr<DB>*`**（非 `DB**`） | API 变更 | 成员 `db_` 从 `DB*` 改 `unique_ptr<DB>` |
| L11 | RocksDB default CF handle 泄漏 | 未保存 handle | 加 `cf_default_` 成员 |
| L12 | protobuf 7.x 把 enum 编码进 `AddSerializedFile(b'...')` 二进制串，明文被拆 → `grep "CURRENCY_STATUS_DISABLED"` 返回 0 | 序列化格式 | 运行时动态注入，import 完全可用（实测值=1）。**grep 0 是显示假象，勿误判为生成失败** |
| L13 | `class` 是 Python 保留字，protobuf 7 保留原名 | 语言冲突 | `getattr(cw, "class")` 访问 |
| L14 | flatc 编译 `aoi.fbs` 报 `class_id` 默认 0 不在枚举内 | `ClassId` 原从 `Warrior=1` 起缺 `=0` 成员 | `common.fbs` 补 `None = 0` |
| L15 | `unordered_map<string>::find(string_view)` 在 GCC16 失败 | 库约束 | 先 `std::string k(key)` |
| L16 | 🔴 **`#include <memory>` 写在 namespace 内部** → MODULES=ON 展开污染 std（`std::memset` → `cami::gateway::redis::std::memset`） | include 位置 | 标准头文件**一律放 namespace 外**（文件顶部） |
| L17 | `ratelimit_types.h` 用 `int64_t` 但未 `#include <cstdint>` → 级联出 `now_` 找不到 | 传递包含不可靠 | 凡用定宽整数必显式含 `<cstdint>` |

### 4.4 沙箱 / 本地环境

| # | 现象 | 根因 | 解法 |
|---|---|---|---|
| S1 | ⚠️前后有变化：早期「WorkBuddy 沙箱 MINGW64 未装 cmake/g++/boost，无法本地跑」 | **误判**：代理 shell 初始 PATH 不含 MSYS2 工具链，但 `/c/msys64/mingw64` 内 **cmake 4.4.2 / g++ 16.1.0 / Boost 1.91.0-3 全装好** | `export PATH="/c/msys64/mingw64/bin:/c/msys64/usr/bin:$PATH"` 即可编译 |
| S2 | MSYS2 必须在 **MINGW64 终端（mingw64.exe）** 编译 | 终端选择 | base MSYS 终端 `/mingw64/bin` 不在 PATH |
| S3 | **safe-delete 包装**：`rm -rf <相对路径>` 报 "relative path rejected" 并让整条 `&&` 链失败 | 安全策略 | 用带盘符绝对路径 `F:/AI/workbuddy/CAMI/xxx`（POSIX `/f/AI/...` 被拒）；**大目录 `rm -rf` 仍被拦截 → 用新 build 目录绕过**；`cmd /c` 被禁；**git 操作别用 `&&` 串在 rm 后面** |
| S4 | **GTest 是 MSYS2 动态库**，直接跑 exe 报 `libgtest_main.dll not found` | PATH | 跑 ctest 前先 export PATH（同 S1） |
| S5 | 仓库残留 `NUL`（Windows 保留设备名，0 字节） | 历史误操作 | git 提交不了，无害，忽略 |
| S6 | git-bash heredoc `<<'EOF'` 在沙箱下解析失败；Write 工具写临时脚本会被沙箱隔离找不到 | 沙箱限制 | 用 Edit 工具直接改，或 `python -c` 单行 |
| S7 | 🔴 `wsl` 裸进落到 **`docker-desktop` WSL2 工具分发**（`/etc/os-release` = "Docker Desktop"，无 apt） | 默认分发是 docker-desktop | **必须显式 `wsl -d Ubuntu`**；`wsl -l -v` 列全部分发；docker-desktop 分发切勿装工具链 |
| S8 | ⚠️前后有变化：WSL `sudo apt update` 全 `Temporary failure resolving` → 先误诊 DNS → 实为**根本没网** | **`.wslconfig` 的 `networkingMode=mirrored` 在 iPhone 热点网段（172.20.10.x）下网络初始化失败**（只有 lo、无 eth0） | `Move-Item .wslconfig .wslconfig.bak` + `wsl --shutdown` + 重开 → 回归默认 NAT。**教训：WSL 只有 lo 无 eth0 时，先怀疑 `.wslconfig` 镜像模式** |
| S9 | Store 版 WSL 不注册 `LxssManager` 服务 | 安装渠道 | 重置靠 `wsl --shutdown` 即可 |
| S10 | WSL 能 ping + apt 正常，但 Tsinghua git 源 443 超时 | **WSL2 经 vEthernet NAT 出网，不继承 Windows 浏览器代理** | `curl -4` 先排除 IPv6 黑洞；需代理则查 Win 代理地址 + 开 Clash Allow LAN |
| S11 | 🔴 agent 侧 `git ls-remote git@github.com:...` 沙箱内外均失败 | **agent runtime 无 GitHub 出网**（`dangerouslyDisableSandbox=true` 也一样） | 装 vcpkg / 跑本地 MODULES=ON 等联网操作**只能交给用户在本机执行** |
| S12 | MSYS2 自带 `ssh-keygen` 路径解析失败 | MSYS2 版本问题 | 用 **Windows 原生 OpenSSH** 并传 Windows 路径；shell 缺 `chmod`，`&&` 链会断，勿加 chmod |
| S13 | 用户 MSYS2 终端多行 `\` 续行被吞（同 V5） | 终端行为 | 单行命令，或写进 CMakeLists / 脚本文件 |
| S14 | 压测 `run_in_background` 沙箱异常 | 沙箱限制 | 用同条前台命令 `&` + `$!` + kill/wait |
| S15 | 🔴 网络探测全景：`github.com:22`✅ / `github.com:443`❌ / `ghproxy.com:443`❌ / `kgithub.com:443`❌ / `gitclone.com:443`✅(clone 403) / `raw.githubusercontent.com:443`✅ / `codeload.github.com:443`✅ / `gitee.com:443`✅ / `mirrors.tuna.tsinghua.edu.cn:443`✅(Windows 侧通，WSL 内超时) | 本机无任何代理在听（7890/7891/1080/10808/10809/8080/8888 全 closed） | **本地 vcpkg install 在无代理/换网前无解**；SSH 22 只能 clone；bootstrap 可用 `-useSystemBinaries` 免下载 |

### 4.5 数据库 / ShardingSphere / Docker

| # | 现象 | 根因 | 解法 |
|---|---|---|---|
| D1 | SS 5.5.0 **代理认证仅认 `root`/`root`** | 5.5.0 特有 | `server.yaml` 用 `root@%`/`root`；后端分片密码仍是 `cami_dev_2026` |
| D2 | SS 5.5.0 规则文件名**必须**为 `config-<databaseName>.yaml` | 命名约定 | 改 `docker/shardingsphere/config-cami_db.yaml` |
| D3 | SS 5.5.0 下 `autoTables` 的 `actual_data_nodes` 为空 | 5.5.0 bug/限制 | 改用 `tables` + `INLINE` 算法，显式 `actualDataNodes` |
| D4 | `verify_mysql_sharding.sh` 重复运行从 15/15 变 14/15 FAIL | **脚本幂等性缺陷**：测试数据只在末尾删除，重复运行时 INSERT 命中重复主键 → `[ $? -eq 0 ]` 误报（路由数据本身正确） | 步骤 3 同步直连两分片清理夹具 |
| D5 | SS 5.2+ 读写分离 YAML 语法变更，代理启动崩 `Unable to find property 'type'` | 语法变更 | 删 `type:Static`/`props:`，改 `writeDataSourceName` + `readDataSourceNames:`（列表） |
| D6 | 改 SS 配置后 `up -d` 不生效，旧容器仍跑旧算法 | `up -d` 不比对挂载文件内容 | 必须 `docker compose up -d --force-recreate shardingsphere-proxy` |
| D7 | MySQL 从库带 `--super_read_only=ON` 启动 → 复制控制语句被拦、容器中止 | 启动参数 | 裸启动后动态 `SET GLOBAL read_only=ON`；GTID 用 `MASTER_AUTO_POSITION=1` |
| D8 | Docker NAT 隔离，宿主 Windows 连不上发布端口（WinError 10061） | 网络隔离 | 改用**容器内 mysql 客户端**直连真实主库测 TPS |
| D9 | 🔴 **数字修正**：`docker compose exec` 并发 4 路得 **727 TPS** 是**编排开销假象，非 DB 上限** | 测量方法错 | 规范测法 = 单连接直插 → **9784 TPS** |
| D10 | 沙箱无 mysql 客户端 | 环境 | `docker compose exec -T <svc> mysql -uroot -pcami_dev_2026 <db> < script.sql`；init 脚本**仅空卷首启执行** |
| D11 | 🔴 **P0 schema 契约错配**：`player_base` 无 `payload` 列，而 `mysql_backing_store` 引用它 → 运行时 `Unknown column`；`REPLACE INTO` 会清空 `account_id`（NOT NULL 无默认 → INSERT 失败） | 设计与实现脱节 | 新建 `player_state(player_id,payload,version)` 分片表，四路径 SQL（Load 带版本 / Store UPSERT version+1 / CasStore `UPDATE WHERE version` 判 affected_rows / Delete）。**方案 B `b08affd` 已实现** |
| D12 | MySQL `Load` 二进制长度截断 | C API 陷阱 | `mysql_fetch_lengths` 取真实长度 |
| D13 | SS 配置含 `!SHARDING` tag，PyYAML 报 unknown tag | YAML 自定义 tag | 自定义 loader |
| D14 | `grokzen/redis-cluster:7.0` 在 Docker Hub **不存在**（7.x 仅 `7.2.5`） | 镜像 tag | 改 `:7.2.5`。**服务容器镜像 tag 提交前应先去 Docker Hub 核对** |

### 4.6 网络 / Git / CI

| # | 现象 | 根因 | 解法 |
|---|---|---|---|
| N1 | ⚠️前后有变化：`ssh.github.com` 22/443 **均被墙**；**仅 `github.com:22` 与 `github.com:443` 通** | 大陆网络 | remote 用 **SCP 形式** `git@github.com:shengmingaini/CAMI.git`，绕过全局 insteadOf |
| N2 | Windows git 全局 `insteadOf` 把 `https://github.com/` 改写为 `ssh://git@ssh.github.com:443/`（被墙）→ 所有 https github clone 全挂，报错信息误导 | 历史配置副作用 | `git config --global --unset-all url."ssh://git@ssh.github.com:443/".insteadOf` 删除。项目 remote 是 SCP 形式，**删除不影响 push** |
| N3 | 🔴 **GitHub 账户计费失败**（2026-08-14）：三 job 全红、总时长 8s、无 step 日志 | > "The job was not started because recent account payments have failed or your spending limit needs to be increased." **与代码完全无关** | 免费额度（约 2000 分钟/月）被反复全量 vcpkg 构建（首次 ~1.5h）耗尽。判断标志 = **"job was not started" 而非某 step 报错** → 先查 GitHub Billing，不要继续改代码 |
| N4 | 🔴 **GitHub "Re-run" 回放旧 commit**（踩了至少两次）：点旧失败 run 的 Re-run，日志照样报同样的错 | Re-run 只是用同一份 commit 重跑，不会凭空出现工作树里没提交的修复 | 必须本地 `git commit` + `git push` 产生**全新 run** |
| N5 | 🔴 **CI #2 双红的推测→推翻**：先推测 economy 红 = pip 版本冲突 / build 红 = Boost 缺依赖 → 用户贴出真报错后**推翻**：economy 真因是 protoc 整批生成失败却未冒泡；build 真因是 `Could not find boost_systemConfig.cmake` | 推测 vs 实测 | **报错必须取 step 级原文，不要靠推测**。解法 = 把已验证产物（22 个 `_pb2.py` 桩）入库 + apt 装 `libboost-system-dev` |
| N6 | 🔴 **"本地能过 / CI 必红"通用解法**：`verify/load_configs.py` 用硬编码 Windows venv 绝对路径定位 protoc → runner 上两路径都不存在 → RuntimeError | 硬编码机器专属路径 | ① candidates 优先 `[sys.executable]`，仅 win 时回退受管 venv；② **优先把已验证产物入库消除环境差异**；③ `subprocess.run(capture_output=True)` 后 `returncode!=0` 显式抛错 |
| N7 | CI YAML 报 "yaml syntax on line 104"，实为 line 106 | `run: "$VCPKG_ROOT/vcpkg" install` —— YAML 把 `"$VCPKG_ROOT/vcpkg"` 当闭合双引号标量，后面 ` install` 成悬空二次 token | 改 plain 标量 `run: $VCPKG_ROOT/vcpkg install`。**纪律：改完 workflow 用本地 PyYAML `yaml.safe_load` 先解析**（GitHub 行号会偏 ~2 行） |
| N8 | 🔴 **GitHub 对未认证请求隐藏 private 仓库**：`curl api.github.com/repos/.../status` 返回 **404** | private 仓库 | CI 结果须由用户在网页 `github.com/shengmingaini/CAMI/actions` 确认 |
| N9 | 🔴 **明文凭据泄露事件已发生**：用户曾将 GitHub PAT 明文粘贴进对话 | — | 已要求立即 Revoke（**用户已执行**）+ 写入用户级红线。⛔ **该红线必须延续到未来所有会话** |
| N10 | 🔴 **branch protection 会把自己锁在门外**：当前工作流是直接 push 到 `main`；开「Require a pull request before merging」会拒绝直接 push | 工作流不匹配 | **路线 B（已采用）**：暂不开保护，直接 push + CI 每次自动跑、红即知 |
| N11 | CI 触发分支仅 `main/master/dev`（+ `workflow_dispatch`） | ci.yml 配置 | 08-11 起已改 **dev 为开发基线** → 问题自然消失 |
| N12 | 私有仓库无 `gh` CLI / 无 GH_TOKEN | 凭据红线 | PR 由用户在网页创建，或本地 `git checkout dev && git merge --no-ff <feature>` 后 push dev |
| N13 | 🔴 **行尾（CRLF/LF）噪声**：`git status` 显示 ~50 文件 modified，但 `git diff HEAD -w` 仅 4 文件有真实改动 | 无 `.gitattributes`、`core.autocrlf` 未设 | 每次提交**只 stage 指定路径**（绝不用 `git add -A`/`.`）；独立提交 `94aba22`（`.gitattributes` `* text=auto eol=lf` + renormalize）。⚠️ 副作用：中间产生占位提交 `e25131e` "feat: xxx"（702 增/702 删，实为行尾变更） |
| N14 | 🔴 **CI #2 修复从未提交**：用户点 Re-run 后报错一字不差，实为三处修复从未 commit | 同 N4 | 见 N4 |
| N15 | GitHub raw logs 报 Azure `BlobNotFound` | GitHub 日志服务临时故障 | 用 run 的 **Download logs 按钮**下载，不要点 raw URL |

### 4.7 🔴 代码级隐藏 bug（全部仅旧日志有）

| # | 现象 | 根因 | 解法 |
|---|---|---|---|
| B1 | `security_selfcheck()` 独立编译返回 0（通过），但链式 `cami_skeleton_check` 返回 1（CI 判失败），且内部全打印 `[ OK ]` | **返回值语义不一致**：旧实现返回 `int`（0=成功，main 风格），骨架调用 `if (!security_selfcheck())` 期望 `bool`（true=成功）→ `!0 == true` 成功反被判失败。其余四个兄弟 selfcheck 均返回 `bool` | 改返回 `bool`（true=ok）。**排查路径值得记**：排除了陈旧 .obj / 编译 flag / .a 符号污染 / 其他 lib 干扰 / 调用顺序，最后才发现是返回值语义 |
| B2 | Connection::on_data 闭包捕获 `shared_ptr<Connection>` → **引用环** → 永不释放 | 生命周期环 | 闭包用**裸 this 指针** |
| B3 | `FlushNow` 早期只对已存在行 Cas，新行必失败 → 落库 0 条 | 逻辑缺陷 | 新行走 `vstore_.Init`、已存在走 `vstore_.Cas(version)` |
| B4 | Kafka DLQ 成功后未提交 offset → 主 topic 卡死重放 + DLQ 无限重复 | 逻辑缺陷 | DLQ 成功后提交 offset |
| B5 | `CacheProxy::Delete` 用 `Store(key, "")` hack → MySQL `REPLACE ''` 行未删 | 语义错误 | MySQL 空值 → `DELETE`；InMemory 空值 → erase |
| B6 | `VersionedStore::Set`（`Put` 无条件版本写）→ 缓存/版本不一致 | 语义错误 | `Put` 改为带版本写 |
| B7 | 信号处理器直接 Shutdown → 死锁 | 信号安全 | 只置 flag，主线程轮询后 Shutdown |
| B8 | `MGet` 未用真 `mget`（N 次 RTT） | 性能 | 真 `mget` 单 RTT |
| B9 | DataClient 无 deadline | 健壮性 | 每 RPC `set_deadline(2s)` |
| B10 | MySQL 每 key prepare | 性能 | 5 个 stmt 预编译缓存（upsert/delete/cas_upd/cas_sel/cas_ins），重连 `CloseStmts` |
| B11 | `std::vector<uint8_t>{4096}` 触发 `-Wnarrowing` | 初始化语义 | 改 `std::array<uint8_t,4096>` |
| B12 | `boost::asio::write` 未显式 include | IWYU | 加 `#include <boost/asio/write.hpp>` |
| B13 | `printf` 把 `int` 传给 `%.0f` 导致 `avg=0` | 类型不匹配 | 传 `double` |
| B14 | `std::vector<uint8_t>{}` 未 include `<vector>` 等 | IWYU | 显式 include（见 L17） |
| B15 | ContentRegistry 单例跨测试污染 → 绝对 ActiveCount 断言失败 | 测试设计 | 用独立模块 + **相对差值**断言 |
| B16 | `NetworkId` 含 uint64 致 8B 对齐 → `sizeof==16`（非 12） | 对齐 | 改 `static_assert(sizeof==16)`，准确表达非隐藏开销 |

---

## 5. 待办清单

### P0（阻塞主线 / 有正确性风险）

| # | 待办 | 阻塞原因 |
|---|---|---|
| P0-1 | **推送 `34f0fd3` 到 origin/dev**（本地领先 1 笔） | 未推送；`origin/dev` = `f13f973`。2026-08-29 实测 `github.com:22` 通，可直接推 |
| P0-2 | **解决 GitHub Actions 计费/额度问题**，恢复 `build-modules-on` job | 账户支付失败或消费限额；免费额度被反复全量 vcpkg 构建耗尽。用户明确拒绝付费依赖 CI |
| P0-3 | **msquic / grpc / protobuf 从未真编译过** → `quic_transport.cpp`、`data_service/`（gRPC）只做过语法检查，无链接验证 | 本地 MinGW 装不了 msquic；grpc 极难编且未装；CI 因 P0-2 不可用 |
| P0-4 | **`RedisBackend` 加 standalone / cluster mode 开关** | Scale-to-Fit 的 Redis 单实例起步依赖此开关，当前只有 cluster 实现 |
| P0-5 | **玩法模块迁移至 ECS SoA + Cap'n Proto** | W1/W3 只建了基础设施，`game/{character,combat,aoi,scene,quest,economy,social}` 仍为空目录 |
| P0-6 | **网关 connection 采用 ITransport 接入 ConnectionMigrator** | 现有 Connection 直接持有 `tcp::socket`，未走 ITransport 抽象，W5 迁移治理器尚未接入热路径 |
| P0-7 | **capnp 编译器未装** → `proto/capnp/` 10 schema 从未编译校验 | 本地 vcpkg 已装 capnproto，但 schema 编译步骤未跑 |
| P0-8 | **修复 `main` 分支严重落后**（停在 `ba8ec30`，落后约 37 commit） | 需用户决定合并策略（注意 N10：开 branch protection 会锁死直接 push） |

### P1（功能缺口）

| # | 待办 | 阻塞原因 |
|---|---|---|
| P1-1 | 内容模块化：`content_phase.json`、升级→ModuleUnlockCheck→懒加载事件链、对象池按模块预分配、`ConfigSource` 的 MODULES=ON 真实现 | 需 protobuf 环境 |
| P1-2 | 网关 pool / 游戏层线程与对象池 / L1 缓存容量配置化 → `configs/scale.json` 骨架 | Scale-to-Fit P0 代码待办 |
| P1-3 | Kafka lz4 压缩 | 需 CI 真编译确认 |
| P1-4 | 多副本 CAS 的生产多副本实演 | 需 Docker 环境 |
| P1-5 | RocksDB 真实后端实跑（三态机只在仿真双端验过） | 本地已装 rocksdb，未跑真实 DB 集成测试 |
| P1-6 | 落库 Kafka 异步 + 死信的生产验证 | 需 Kafka 服务 |
| P1-7 | 8 分库并行 sysbench | 需 Docker / 专用机 |
| P1-8 | 单机 5 万长连接 × 30 分钟全量压测 | **沙箱跑不出**（Windows 默认动态端口 ~1.6 万、FD/时长受限）；需专用调优 Linux 主机。当前只有 harness + 缩量验证（2000 连接/20s、500 静默连接 5030ms 全踢） |
| P1-9 | 800ms 连接迁移实测 | 需真实多网关压测 |
| P1-10 | 缓存命中率 / 经济平衡线上监控面板接 `alert_channel` | 需运维基础设施 |
| P1-11 | 战斗 DPS/TTK 蒙特卡洛深化（role 隔离 + 次属性建模；当前 WARRIOR==ROGUE 在纯白字基线完全相等） | 需 `combat.md` §1.2 权威化缺失常量 |
| P1-12 | 宝石/附魔/套装数值 + 其 `EconomyFlow` | 用户 08-10 指示"只列出来，数值后续做" |
| P1-13 | `repair_rates` 损耗率仍为**建模假设**（均匀损耗） | 需线上 telemetry 回填 |
| P1-14 | `pending` 协议域：背包 / 场景 / 寻路 / 跨服 | Day3 分期策略已声明 deferred |
| P1-15 | Docs：`economy-balance.md`（economy 确立"发奖唯一出口"但无源/汇平衡模型） | Day3 GAP-4 延伸 |

### P2（优化 / 清理）

| # | 待办 |
|---|---|
| P2-1 | CI build job 默认 `CAMI_BUILD_TESTS=OFF` → GTest 不进 CI。要 Linux 也跑需加 `CAMI_BUILD_TESTS=ON` + `apt install libgtest-dev` |
| P2-2 | 清理空目录 `gateway/migration/` |
| P2-3 | 清理 `build-*` 本地构建目录（均被 `.gitignore` 覆盖，可 `git clean -fdx` 自清） |
| P2-4 | 清理残留 `NUL` 文件 |
| P2-5 | 删除废弃分支 `feat-gap7`（远端已 gone） |
| P2-6 | `.wslconfig.bak` 归档或确认删除 |
| P2-7 | `game/{navigation,collision,prediction}` 目录已建但无代码（v2.0 ADR-005/006 设计未落地） |
| P2-8 | `gateway/migration` vs `docs/design/connection-migration.md` 归属待理清 |

---

## 6. 风险与已知未验证项

### 6.1 🔴 从未真编译 / 从未真实链接的代码

| 代码 | 门控 | 验证状态 | 阻塞 |
|---|---|---|---|
| `gateway/transport/quic_transport.cpp` | `find_package(msquic QUIET)` + `#ifdef CAMI_BUILD_MODULES` | **从未编译过**。0-RTT + ConnectionID 无状态迁移的**真绑定尚未实现** | msquic 是 Windows 原生，MinGW 装不了；CI 因 P0-2 不可用 |
| `data/data_service/*`（gRPC） | `find_package(Protobuf/gRPC QUIET)` 双守卫 | **从未编译过**。做过静态语法检查（proto3 标准，判定兼容） | grpc 在 MinGW 极难编且未装 |
| `data/rocksdb_proxy/rocksdb_backing_store.cpp` | 已装 rocksdb 并编译 | **已编译链接，但从未跑真实 RocksDB DB** | 需集成测试环境 |
| `data/sync/kafka_flush.cpp` | cppkafka | 做过本地语法检查（API 差异已修），**未链接、未实跑** | 本地 MinGW 装 cppkafka 中途网络断 |
| entt `ecs_world.h` | 已装 entt 并编译 | **已验证**（`ecs_world_test` 在 18/18 内） | — |

**🔴 关键教训**：`libmysql`/`cppkafka` 的 `find_package` 包名写错 → `QUIET` **静默跳过** → 构建"绿"但后端**根本没编译**。**这比编译报错更隐蔽**——纸面绿的 MODULES=ON 不代表后端被验证。`docs/architecture/modules-on-preflight.md` 的静态预检正是为此而做。

### 6.2 只在特定路径下验证过的代码

| 代码 | 验证路径 | 未验证路径 |
|---|---|---|
| `gateway/connection` 的 `SO_REUSEPORT` 多 acceptor | 仅 GitHub CI ubuntu runner（Linux 分支） | Linux 分支**从未在本地跑过** |
| `gateway/security` AES-GCM 真实路径 | 从未验证（MODULES=ON 依赖 OpenSSL） | MODULES=OFF 下是 DISABLED 占位 |
| `gateway/redis` RedisClusterState 真实后端 | CI 用 `grokzen/redis-cluster:7.2.5` 服务容器 | 本地无 Docker 时 **GTEST_SKIP** |
| MySQL stmt 预编译缓存（5 个 stmt） | **待 CI 真编译确认** | MODULES=ON 未跑 |
| Kafka lz4 压缩 | 未验证 | — |
| 网关零拷贝帧视图约束 | 已文档化，靠人工遵守 | 无自动化约束检查 |
| 数据层热路径优化（stats atomic / HotKeyDetector 采样 / dirty 存 {key,value}） | OFF 13/13 回归 | MODULES=ON 未跑 |
| Cap'n Proto 10 schema | 仅 `scripts/capnp_lint.py` 结构护栏 | **无编译器，从未编译** |

### 6.3 🔴 环境级硬约束（影响所有未来会话的工作方式）

1. **agent runtime 无 GitHub 出网**：`git ls-remote git@github.com:...` 沙箱内外均失败。装 vcpkg / 跑本地 MODULES=ON 等联网操作**只能交给用户在本机执行**。
2. **本机网络：22 通 / 443 墙**，无任何代理在听。`github.com:443` 超时 → 本地 vcpkg install 依赖源码（走 443）**无解**。
3. **GitHub Actions 当前不可用**（计费/额度，P0-2）→ 唯一可用的权威验证路径暂时断了。
4. **Windows MinGW 物理墙**：msquic 装不了；grpc 极难编。libmysql 曾受阻但换 libmariadb 后已解决。
5. **沙箱跑不出 5 万真实 TCP 连接**（默认动态端口 ~1.6 万、FD/时长受限）→ 全量压测必须专用调优 Linux 主机。
6. **Docker 可用**（29.6.2）但宿主 Windows 连不上发布端口（NAT 隔离），须用容器内客户端。

---

## 7. 未来会话的最小启动清单

1. `cd F:/AI/workbuddy/CAMI && git log --oneline -3 && git status` — 确认是否还在 `dev`/`34f0fd3`
2. `git push origin dev` — 补推 `34f0fd3`（P0-1）
3. 编译前 `export PATH="/c/msys64/mingw64/bin:/c/msys64/usr/bin:$PATH"`（否则 cmake/g++/GTest DLL 全找不到）
4. OFF 快速回归：`cmake -B build-x -G Ninja -DCAMI_BUILD_MODULES=OFF -DCAMI_BUILD_TESTS=ON && cmake --build build-x && ctest --test-dir build-x --output-on-failure` → 期望 **17/17**
5. 经济校验：`python verify/balance_verifier.py` → 期望 exit 0 / **PASS=59 WARN=0 FAIL=0**
6. 改 CI workflow 前先 `python -c "import yaml; yaml.safe_load(open('.github/workflows/ci.yml'))"`
7. **绝不 `git add -A`/`.`**（行尾噪声 ~47 文件）
8. **⛔ 绝不要求用户贴明文 PAT/凭据**（历史上已泄露一次并 Revoke）
9. **改数值前先配 `balance.json.economy_flows` 且被 verifier 覆盖**
10. **模块间仅经 EventBus 或 gRPC，禁止直接引用其他模块内部类**
