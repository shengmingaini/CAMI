# CAMI 总体架构规约文档

> **文档状态**: [PRODUCTION]  
> **版本**: v3.0.0  
> **更新日期**: 2026-08-06  
> **符合5万在线架构**: 是  
> **v3.0变更**: 高并发优化 — 共享内存IPC/场景线程+Job System+ECS/三级缓存+WAL/QUIC+边缘网关/多级背压+Cell分片/AOI预测预取+差分压缩+跨节点聚合

---

## 1. 项目概述

### 1.1 目标

构建单集群5万并发在线的工业级MMORPG后端系统，支持线性扩容，遵循"四无"原则：无单点、无全局锁、无同步阻塞IO、无全服广播。

### 1.2 核心约束

| 约束 | 指标 |
|------|------|
| 单集群并发在线 | 50,000 |
| 单GameNode承载 | 1,000人 |
| 集群最小节点数 | 50个GameNode |
| 战斗循环耗时(1000人场景) | <=5ms |
| 单节点消息吞吐 [v3.0] | <=20,000条/秒 |
| Gateway↔GameNode延迟 [v3.0] | <0.05ms (共享内存) |
| 热数据读取延迟 [v3.0] | <0.01ms (L1缓存) |
| 数据写入延迟 [v3.0] | <0.1ms (WAL) |
| 客户端重连延迟 [v3.0] | ~0ms (QUIC 0-RTT) |
| L1缓存命中率 [v3.0] | >=95% |
| CPU利用率 [v3.0] | >=70% |
| 跨节点AOI消息量 [v3.0] | <=10条/s |
| 网关数量 | >=10 (边缘+中心双层) |
| Redis分片数 | 16 |
| MySQL分库数 | 8主 + 8从 [v3.0] |
| 带宽预留 | 12Gbps |

### 1.3 红线禁令

1. 禁止单体架构
2. 禁止战斗循环内写DB
3. 禁止JSON传高频战斗数据
4. 禁止无AOI全服广播
5. 禁止无版本号多节点并发写玩家数据
6. 禁止全局锁/单点定时器
7. 禁止模块间直接访问内部成员

---

## 2. 技术栈

| 层级 | 技术 | 版本 | 用途 |
|------|------|------|------|
| 核心逻辑 | C++ | C++17 | GameNode / Gateway / DataService 核心实现 |
| 玩法热更 | Lua | 5.4 | 技能/任务/配置逻辑热更 |
| Lua绑定 | Sol2 | 3.x | C++ ↔ Lua 交互 |
| 实时数据 | Redis Cluster | 7.x | 在线状态/背包/货币/Buff 缓存 |
| 持久化 | MySQL | 8.0 | 玩家/公会/拍卖行 持久化 |
| 分库分表 | ShardingSphere | 5.x | MySQL 分片路由管理 |
| 高频序列化 | FlatBuffers | latest | 网络/战斗消息序列化 |
| 配置序列化 | Protobuf | 3.x | 配置表/跨服协议 |
| 异步队列 | Kafka | 3.x | 邮件/日志/排行榜异步持久化 |
| 容器编排 | K8s | 1.28+ | 集群编排与HPA自动扩缩容 |
| 服务网格 | Istio | 1.20+ | east-west流量治理/mTLS |
| 构建系统 | CMake | 3.20+ | C++构建与依赖管理 |
| 依赖管理 | vcpkg | latest | C++第三方库管理 |
| RPC框架 | gRPC | 1.60+ | 跨服务通信 |
| 监控 | Prometheus + Grafana | latest | 指标采集与可视化 |

---

## 3. 架构总览

### 3.1 五层架构

```
┌──────────────────────────────────────────────────────────────────┐
│                       运维控制层 (Ops)                            │
│  节点编排 │ Lua热更 │ 灰度发布 │ 配置中心 │ 故障自愈               │
├──────────────────────────────────────────────────────────────────┤
│                     全局公共服务层 (Common)                       │
│  分布式定时器 │ 结构化日志 │ 监控告警 │ 排行榜 │ 防外挂            │
├──────────────────────────────────────────────────────────────────┤
│              数据管理层 (Data Service) [v3.0 优化]                │
│  Redis缓存代理 │ MySQL分库分表代理(+读写分离) │ WAL(+Write-Behind) │
│  数据同步 │ 版本校验                                              │
│  [v3.0] DataService负载降低90% — GameNode直连Redis + L1缓存       │
├──────────────────────────────────────────────────────────────────┤
│              逻辑业务层 (Game Node) [v3.0 增强]                   │
│  场景调度(+Phase+Layer+Cell分片) │ 角色(+ECS SoA布局)            │
│  战斗(+批次+LOS) │ AOI(+LOD+预测预取+差分压缩+跨节点聚合)        │
│  社交 │ 任务 │ 交易经济 │ 寻路 │ 碰撞 │ 预测                     │
│  [v3.0] 场景级线程隔离 + Job System + 多级背压 + L1缓存           │
├──────────────────────────────────────────────────────────────────┤
│              接入层 (Gateway) [v3.0 增强]                         │
│  连接管理(QUIC) │ 协议编解码(+位压缩+消息聚合) │ 安全校验         │
│  消息路由 │ 连接迁移(0-RTT)                                       │
│  [v3.0] 边缘网关(就近接入) + 中心网关 + 共享内存IPC              │
└──────────────────────────────────────────────────────────────────┘
```

### 3.2 依赖方向（严格单向，禁止逆向）

```
[v3.0] 边缘网关 ──→ 中心网关 ──→ GameNode ──→ DataService ──→ Redis / MySQL
                        │            │              │
                        │            ├──→ Common Services (gRPC)
                        │            │
                        │            ├──→ Redis直连 [v3.0] (L1缓存未命中时)
                        │            │
                        └──→ 中心网关 ──→ GameNode (共享内存IPC [v3.0])
    
Ops ──→ 所有层（控制面，不走数据面）
```

**规则**:
- 上层只能调用下层接口，下层不能回调上层
- 同层模块间通过进程内事件总线通信，禁止直接引用
- 跨服务通信优先使用共享内存IPC（同节点），其次gRPC（跨节点） [v3.0]
- GameNode可直连Redis读取热数据（L1缓存未命中时），但MySQL写入仍必须走DataService [v3.0]

### 3.3 通信矩阵

| 通信场景 | 方式 | 延迟目标 | 备注 |
|---------|------|---------|------|
| GameNode 内模块间 | 进程内 EventBus | <0.01ms | publish/subscribe, 零序列化 |
| Gateway ↔ GameNode (同节点) [v3.0] | **共享内存 SHM + RingBuffer** | **<0.05ms** | 零拷贝, 零序列化, 替代gRPC |
| Gateway ↔ GameNode (跨节点) | gRPC (批量) | <1ms | [v3.0] 批量发送减少RPC次数 |
| GameNode ↔ Redis [v3.0] | **Redis直连 (CRC16分片)** | **<0.5ms** | L1未命中时直连, 绕过DataService |
| GameNode ↔ DataService | gRPC | <2ms | 仅MySQL写入和非热数据读取 |
| GameNode ↔ Common Services | gRPC | <1ms | 定时器/日志/排行榜 |
| 异步任务 (邮件/日志) | Kafka | 秒级 | 不阻塞主循环 |
| 跨服事件 (预留) | Redis Pub/Sub | <5ms | 跨服战场/跨服聊天 |
| 客户端 ↔ 边缘网关 [v3.0] | **QUIC (0-RTT)** | **~0ms重连** | 替代TCP, 支持连接迁移 |
| 客户端 ↔ Gateway (高频) | UDP/QUIC + 位压缩 | - | 位置/技能/AOI, 自定义编码 |
| 客户端 ↔ Gateway (可靠) | QUIC + FlatBuffers | - | 登录/交易/任务等 |
| GameNode 内寻路查询 | 进程内调用 | <0.5ms | NavMesh A* |
| GameNode 内LOS查询 | 进程内缓存 | <0.001ms | 缓存命中O(1) |
| GameNode 内碰撞检测 | 进程内调用 | <0.1ms | 动态AABB树 |
| 法术批次结算 | 进程内批次 | 100-200ms | PVP场景启用 |
| 跨节点AOI [v3.0] | **聚合+差分 (gRPC)** | <1ms | 消息量从30000/s降至10/s |
| GameNode内L1缓存查询 [v3.0] | **进程内L1缓存** | **<0.01ms** | 命中率>=95%, SoA布局 |
| 数据写入WAL [v3.0] | **进程内WAL** | **<0.1ms** | 异步批量落库, 零数据丢失 |
| 场景线程间通信 [v3.0] | **MPMC无锁队列** | <0.01ms | 跨场景消息传递 |

---

## 4. 详细架构设计

### 4.1 接入层 (Gateway)

**职责**: TCP/UDP/QUIC连接维持、消息加解密、心跳、限流、防攻击。零玩家持久化数据存储。

**模块拆分**:

| 模块 | 职责 | 关键设计 |
|------|------|---------|
| 连接管理模块 | 维持客户端连接、心跳检测、超时断开 | [v3.0] QUIC协议替代TCP, 0-RTT重连, 连接迁移支持移动端网络切换 |
| 协议编解码模块 | FlatBuffers编解码 + 高频消息位压缩编解码 | [v2.0] 双轨: 高频UDP用位压缩, 可靠TCP用FlatBuffers, AES-128-GCM加密; [v3.0] 消息聚合(5ms窗口批量打包) |
| 安全校验模块 | 防刷包、限流、黑名单、协议校验 | 令牌桶限流, 单IP 100req/s |
| 消息路由模块 | 根据消息类型路由到目标GameNode | [v3.0] K8s亲和性路由(同节点优先共享内存), 一致性哈希 |
| 连接迁移模块 | GameNode宕机时迁移玩家连接上下文 | [v2.0] 800ms内完成; [v3.0] QUIC连接迁移0ms(连接ID不变, 底层自动迁移) |
| **边缘网关** [v3.0 新增] | 就近接入, 降低玩家RTT | 多地部署, 边缘网关→中心网关专线转发, RTT降低80% |
| **共享内存IPC** [v3.0 新增] | 同节点Gateway↔GameNode零拷贝通信 | SPSC RingBuffer, 64MB/节点, 延迟<0.05ms |

**扩容**: [v3.0] 边缘网关+中心网关双层架构，均无状态水平扩展。

**容量**: [v3.0] 10+边缘网关(2C4G, 就近接入) + 10+中心网关(4C8G, 每节点5000连接), 总容量50000。

### 4.2 逻辑业务层 (Game Node)

**职责**: 场景调度、角色管理、战斗、AOI、社交、任务、交易经济。

**模块拆分**:

#### 4.2.1 场景调度模块 (Scene Scheduler) [v3.0 增强]
- 场景分片为主、玩家分片为辅
- 动态调度，单节点承载上限：主城3000人、野外1200人、副本500人
- 副本进程独立隔离
- **Phasing（相位）**: 基于玩家任务进度展示不同世界状态，同一坐标不同相位
- **Layering（分层）**: 热门区域自动拆分平行层，单层容量1200人，自动均衡
- **跨服区域**: 低人口区域合并，基于Redis Pub/Sub同步跨节点实体
- **Cell-based开放世界** [v3.0 新增]: 大型开放世界场景分Cell管理（100m×100m），每Cell独立线程，1000人场景分散到25个Cell线程，每线程仅40人
- **场景级线程隔离** [v3.0 新增]: 每场景绑定独立线程，AOI/战斗/寻路在该线程内同步执行，跨场景通信走MPMC无锁队列
- **Job System** [v3.0 新增]: 非关键任务（邮件发送/日志/排行榜更新）通过Job System并行化，不阻塞场景主循环

#### 4.2.2 角色模块 (Character) [v3.0 增强]
- **核心设计 (WoW模式)**: 角色模块拥有Player对象的内存实例，是玩家数据的单一权威源
- 管理内容: 属性、天赋、BUFF、装备、背包、货币、幻化、坐骑宠物
- 对外暴露: 属性查询接口（返回const引用）、属性修改接口（含校验逻辑）
- **读写规则**:
  - 其他模块读取角色属性: 获取 `const Player&` 引用，直接内存访问，零开销
  - 其他模块修改角色属性: 调用 `Player::ApplyDamage()` / `Player::ApplyBuff()` 等方法，角色模块内部校验（无敌/护盾/抗性）
  - Player对象在玩家上线时加载到内存，下线时持久化
- **ECS SoA数据布局** [v3.0 新增]: 高频属性（位置/血量/Buff列表）采用SoA(Structure of Arrays)布局，CPU缓存命中率提升3-5倍，批量遍历1000人属性从O(N)内存随机访问变为顺序访问
- **L1进程内缓存** [v3.0 新增]: 热数据（属性/背包/货币）缓存在GameNode进程内，命中率>=95%，Redis QPS降低90%

#### 4.2.3 战斗模块 (Combat) [v2.0 增强]
- 独立实现: 技能判定、仇恨系统、AOE伤害、PVE/PVP规则、施法中断
- **不直接操作角色数据**: 仅调用角色模块的修改接口
- **读取优化**: 通过 `const Player&` 引用读取属性，战斗循环零接口调用开销
- 写入路径: `CombatSystem::ExecuteSkill()` → `Player::ApplyDamage()` → 角色模块校验 → 修改属性 → 发布事件
- **法术批次处理 (Spell Batching)** [新增]: PVP场景100-200ms窗口内法术统一结算，保证公平性；PVE场景即时结算
- **LOS视线缓存集成** [新增]: 技能命中判定走LOS缓存，命中率>95%，查询耗时<0.001ms

#### 4.2.4 AOI模块 (Area of Interest) [v3.0 增强]
- 算法: 十字链表 + 动态网格
- 对外接口: 视野增删、消息广播
- **不耦合战斗/移动逻辑**: 仅维护EntityID，需要属性时通过角色模块查询
- 广播: 通过事件总线发布视野消息，Gateway订阅转发
- **LOD三级更新** [v2.0]: 
  - LOD0 (0-30m): 每帧更新(33ms)，完整属性(~47B)
  - LOD1 (30-80m): 每100ms更新，关键属性(~10B)
  - LOD2 (80-200m): 每500ms更新，最小属性(~5B)
- **属性压缩** [v2.0]: 按LOD级别裁剪属性集，远距仅发位置+朝向
- **视野锥裁剪** [v2.0]: 基于朝向的270度视野锥裁剪，减少不必要同步
- **动态负载均衡** [v2.0]: 拥挤区域(>100人/网格)自动缩小LOD0范围
- **预测预取** [v3.0 新增]: 基于玩家移动方向和速度预测未来3秒将进入视野的实体，提前预加载属性数据
- **差分压缩** [v3.0 新增]: AOI更新只发送变化量(delta)，同实体的完整属性仅首次发送，后续仅发diff，消息量减少60-80%
- **层次网格** [v3.0 新增]: 近距细粒度网格(10m)、中距中粒度(50m)、远距粗粒度(200m)三级网格，查询效率从O(N)提升到O(1)
- **跨节点AOI聚合** [v3.0 新增]: 跨节点边界实体通过聚合器统一管理，每5秒批量同步一次边界实体状态，消息量从30000条/s降至10条/s

#### 4.2.5 社交模块 (Social)
- 管理: 队伍、公会、好友、聊天、邮件
- 频道消息走分片路由，禁止全服遍历
- 邮件投递走Kafka异步队列

#### 4.2.6 任务模块 (Quest)
- 引擎: 主线/支线/日常/周常/世界任务
- 通过事件监听触发进度更新，禁止轮询全量玩家

#### 4.2.7 交易经济模块 (Economy)
- 管理: 玩家交易、拍卖行、NPC商店、经济风控
- 所有道具产出消耗走统一校验接口，防止数值通胀
- 拍卖行数据使用独立库（非PlayerID分片）

#### 4.2.8 寻路模块 (Navigation) [v2.0 新增]
- **NavMesh导航网格**: 按场景加载/卸载，支持复杂地形路径规划
- **算法**: A* on NavMesh + Funnel算法路径平滑
- **异步寻路队列**: 每帧最多处理50次寻路请求，防止卡帧
- **飞行路径**: 固定航线运输系统
- 性能: 单次寻路 <=0.5ms

#### 4.2.9 碰撞检测模块 (Collision) [v2.0 新增]
- **动态AABB树**: 玩家/怪物/弹道碰撞检测
- **静态碰撞体**: 地形/建筑碰撞，按场景独立管理
- **范围查询**: AOE技能的范围碰撞查询
- **弹道碰撞**: 远程技能/箭矢的raycast检测
- 性能: 单次碰撞检测 <=0.1ms

#### 4.2.10 LOS视线缓存模块 (LOS Cache) [v2.0 新增]
- **缓存策略**: 2米网格量化 + 5秒TTL自动过期
- **空间哈希索引**: O(1)缓存查询
- **集成**: 战斗模块技能命中判定、AOE伤害结算的LOS检查统一走缓存
- 性能: 缓存命中率 >=90%，查询耗时 <0.001ms

#### 4.2.11 客户端预测系统 (Prediction) [v2.0 新增]
- **客户端预测**: 玩家输入本地立即执行物理模拟，零延迟感知
- **服务器验证**: 权威物理模拟，与客户端预测对比，误差>0.5m时发送纠偏
- **延迟补偿**: 服务器侧回溯命中判定，解决高延迟玩家"打不中"问题
- **平滑插值**: 其他玩家位置100ms插值延迟，消除网络抖动视觉影响
- 预测正确率目标: >=95%

**模块间通信**: 统一事件总线，禁止直接跨模块调用内部变量。

[v3.0] **场景线程间通信**: 跨场景消息通过MPMC无锁队列传递，避免锁竞争。

[v3.0] **Job System**: 非关键任务提交到Job System线程池并行执行，不阻塞场景主循环。

```cpp
// 事件总线示例
class EventBus {
public:
    template<typename EventT>
    void publish(const EventT& event);
    
    template<typename EventT, typename Handler>
    void subscribe(Handler&& handler);
};

// [v3.0] 场景线程间无锁队列
class MPMCQueue {
public:
    bool enqueue(Message&& msg);  // 非阻塞
    bool dequeue(Message& out);   // 非阻塞
    size_t size() const;
};

// [v3.0] Job System
class JobSystem {
public:
    void submit(Job&& job);  // 异步执行
    void waitAll();          // 等待所有Job完成（帧末）
};

// 事件类型定义 (FlatBuffers)
struct DamageDealtEvent {
    uint64_t source_id;
    uint64_t target_id;
    int32_t damage;
    DamageType type;
};
```

### 4.3 数据管理层 (Data Service) [v3.0 优化]

**职责**: 统一数据代理，GameNode禁止直连MySQL。 [v3.0] GameNode可直连Redis读取热数据。

**模块拆分**:

| 模块 | 职责 | 关键设计 |
|------|------|---------|
| Redis缓存代理 | 读写Redis Cluster，处理分片路由 | CRC16分片, Pipeline批量操作; [v3.0] GameNode直连Redis(L2), 代理仅处理跨分片事务 |
| MySQL分库分表代理 | 通过ShardingSphere路由SQL | PlayerID分片键, 8分库; [v3.0] 读写分离(8主+8从) |
| 数据同步模块 | Redis → MySQL批量落库 | [v2.0] 背包/货币每30s; [v3.0] WAL+Write-Behind, 5秒检查点, 零数据丢失 |
| 版本校验模块 | 防多节点并发覆盖 | 乐观锁, 每次写入携带版本号 |
| **L1进程内缓存** [v3.0] | GameNode内热数据缓存 | SoA布局, LRU+TTL, 命中率>=95%, 延迟<0.01ms |
| **WAL预写日志** [v3.0] | 写入持久化保证 | 顺序追加写, 5秒检查点落库, 崩溃恢复零丢失 |

**数据一致性策略**:
- 在线状态: 实时写Redis
- 背包/货币: [v3.0] WAL+Write-Behind, 5秒检查点批量落库
- 玩家断线: 立即持久化全部数据 (WAL flush)
- 所有写入: 携带版本号 (CAS机制防覆盖)
- **三级缓存** [v3.0]: L1进程内缓存(命中率95%) → L2 Redis直连(命中率99%) → L3 MySQL(冷数据)
- **崩溃恢复** [v3.0]: WAL重放未落库变更，数据零丢失（v2.0丢失30s数据）

### 4.4 全局公共服务层 (Common Services)

**5个微服务完全独立部署，无状态可水平扩展**:

| 服务 | 职责 | 关键设计 |
|------|------|---------|
| 分布式定时器 | 全局定时任务调度 | 分片定时器, 无单点 |
| 结构化日志 | 日志采集与结构化 | Kafka → ELK |
| 监控告警 | 指标采集与告警 | Prometheus + AlertManager |
| 分片排行榜 | 跨分片排行榜聚合 | ZSet + 分桶合并 |
| 防外挂校验 | 行为分析异常检测 | 异步分析, 不阻塞主循环 |

### 4.5 运维控制层 (Ops) [v3.0 增强]

| 模块 | 职责 |
|------|------|
| 节点编排 | K8s HPA自动扩缩容; [v3.0] Gateway↔GameNode亲和性调度 |
| Lua热更 | 技能/任务配置热更，文件监听 + 状态机重载 |
| 灰度发布 | 10% → 30% → 100% 灰度切流 |
| 配置中心 | 集中配置管理，热下发 |
| 故障自愈 | [v2.0] 负载超85%自动降级非关键功能; [v3.0] 4级背压(GREEN/YELLOW/ORANGE/RED)渐进式降级 |
| **背压系统** [v3.0 新增] | 4级渐进式过载保护: GREEN(正常) → YELLOW(70%, 关闭非关键AOI更新) → ORANGE(85%, 关闭远距同步) → RED(100%, 仅保留核心战斗) |

---

## 5. 数据架构

### 5.1 Redis Cluster (16分片)

| 数据类型 | 分片键 | 存储结构 | TTL |
|---------|--------|---------|-----|
| 在线状态 | PlayerID | Hash | 永久(在线) |
| 背包 | PlayerID | Hash | 永久 |
| 货币 | PlayerID | Hash | 永久 |
| Buff列表 | PlayerID | List | 过期自动清除 |
| 连接上下文 | PlayerID | Hash | 60s (迁移用) |
| 场景AOI缓存 | SceneID | Set | 场景存活期 |

**分片策略**: PlayerID CRC16 % 16

### 5.2 MySQL (8分库 + 独立库)

#### 5.2.1 玩家分片库 (8分库, ShardingSphere管理)

| 表 | 分片键 | 说明 |
|----|--------|------|
| player_base | PlayerID | 角色基础信息 |
| player_inventory | PlayerID | 背包物品 |
| player_currency | PlayerID | 货币记录 |
| player_equipment | PlayerID | 装备槽位 |
| player_quest | PlayerID | 任务进度 |
| player_skill | PlayerID | 技能数据 |
| player_mail | PlayerID | 邮件 |
| player_social | PlayerID | 好友/黑名单 |

#### 5.2.2 独立库 (不按PlayerID分片)

| 表 | 存储位置 | 说明 |
|----|---------|------|
| auction_house | 独立MySQL + Redis | 全局拍卖行 |
| guild | 独立MySQL + Redis | 公会数据 |
| guild_member | 独立MySQL | 公会成员 |
| ranking_snapshot | 独立Redis | 排行榜快照 |

### 5.3 版本控制与并发安全

```cpp
// 每条数据记录携带版本号
struct PlayerData {
    uint64_t player_id;
    uint32_t version;  // 乐观锁版本号
    // ... 其他字段
};

// 写入时CAS校验
// UPDATE player_base SET ..., version = version + 1 
// WHERE player_id = ? AND version = ?
```

---

## 6. 项目目录结构

```
CAMI/
├── docs/                         # 技术文档
│   ├── architecture/             # 架构设计文档
│   │   └── architecture-spec.md  # 总体架构规约 (本文档)
│   ├── modules/                  # 模块设计文档
│   ├── protocols/                # 协议定义文档
│   ├── database/                 # 数据库Schema文档
│   └── ops/                      # 运维文档
├── gateway/                      # 接入层源码
│   ├── connection/               # 连接管理模块
│   ├── codec/                    # 协议编解码模块
│   ├── security/                 # 安全校验模块
│   ├── router/                   # 消息路由模块
│   ├── migration/                # 连接迁移模块
│   ├── quic/                     # QUIC传输层 [v3.0新增]
│   ├── edge/                     # 边缘网关 [v3.0新增]
│   ├── ipc/                      # 共享内存IPC [v3.0新增]
│   └── aggregation/              # 消息聚合 [v3.0新增]
├── game/                         # 逻辑业务层源码
│   ├── scene/                    # 场景调度模块
│   │   └── phasing/              # 场景相位/分层管理 [v2.0新增]
│   ├── character/                # 角色模块
│   ├── combat/                   # 战斗模块
│   ├── aoi/                      # AOI模块 (含LOD分级)
│   ├── social/                   # 社交模块
│   ├── quest/                    # 任务模块
│   ├── economy/                  # 交易经济模块
│   ├── event_bus/                # 进程内事件总线
│   ├── navigation/               # NavMesh寻路模块 [v2.0新增]
│   ├── collision/                # 碰撞检测+LOS缓存 [v2.0新增]
│   ├── prediction/               # 客户端预测与纠偏 [v2.0新增]
│   ├── threading/                # 场景线程管理+Job System [v3.0新增]
│   ├── ecs/                      # ECS SoA数据布局 [v3.0新增]
│   ├── cell/                     # Cell-based开放世界管理 [v3.0新增]
│   └── backpressure/             # 背压系统+自适应降级 [v3.0新增]
├── data/                         # 数据管理层源码
│   ├── redis_proxy/              # Redis缓存代理
│   ├── mysql_proxy/              # MySQL分库分表代理
│   ├── sync/                     # 数据同步模块
│   ├── version/                  # 版本校验模块
│   ├── l1_cache/                 # L1进程内缓存 [v3.0新增]
│   ├── redis_direct/             # Redis直连客户端 [v3.0新增]
│   └── wal/                      # 预写日志+Write-Behind [v3.0新增]
├── common/                       # 全局公共服务层源码
│   ├── timer/                    # 分布式定时器
│   ├── logger/                   # 结构化日志
│   ├── monitor/                  # 监控告警
│   ├── ranking/                  # 分片排行榜
│   └── anti_cheat/               # 防外挂校验
├── ops/                          # 运维控制层源码
│   ├── orchestrator/             # 节点编排
│   ├── hotfix/                   # Lua热更
│   ├── canary/                   # 灰度发布
│   ├── config_center/            # 配置中心
│   └── self_heal/                # 故障自愈
├── proto/                        # 协议定义
│   ├── flatbuffers/              # FlatBuffers schema (.fbs)
│   └── protobuf/                 # Protobuf schema (.proto)
├── lua/                          # Lua脚本
│   ├── skills/                   # 技能逻辑
│   ├── quests/                   # 任务逻辑
│   └── config/                   # 配置数据
├── scripts/                      # 脚本
│   ├── build/                    # 构建脚本
│   ├── benchmark/                # 压测脚本
│   └── chaos/                    # 故障演练
├── tests/                        # 测试
│   ├── unit/                     # 单元测试
│   ├── integration/              # 集成测试
│   └── e2e/                      # 端到端测试
├── docker/                       # Docker配置
│   ├── docker-compose.yml        # 本地开发环境
│   └── Dockerfile.*              # 各服务Dockerfile
├── cmake/                        # CMake模块
│   └── Find*.cmake               # 自定义Find模块
├── CMakeLists.txt                # 根CMakeLists
├── vcpkg.json                    # vcpkg依赖清单
└── README.md                     # 项目说明
```

---

## 7. 构建与开发

### 7.1 构建系统

```bash
# 构建 (Debug)
cmake -B build/Debug -DCMAKE_BUILD_TYPE=Debug -DCMAKE_TOOLCHAIN_FILE=<vcpkg>/scripts/buildsystems/vcpkg.cmake
cmake --build build/Debug -j$(nproc)

# 构建 (Release)
cmake -B build/Release -DCMAKE_BUILD_TYPE=Release -DCMAKE_TOOLCHAIN_FILE=<vcpkg>/scripts/buildsystems/vcpkg.cmake
cmake --build build/Release -j$(nproc)

# 运行测试
ctest --test-dir build/Debug --output-on-failure
```

### 7.2 依赖管理 (vcpkg)

```json
// vcpkg.json
{
  "name": "cami",
  "version": "0.1.0",
  "dependencies": [
    "grpc",
    "flatbuffers",
    "protobuf",
    "redis-plus-plus",
    "sol2",
    "lua",
    "fmt",
    "spdlog",
    "gtest",
    "boost-asio",
    "openssl"
  ]
}
```

### 7.3 本地开发环境 (混合模式)

```bash
# 启动依赖服务 (Docker)
docker compose -f docker/docker-compose.yml up -d

# 包含: Redis Cluster (16分片), MySQL x8, Kafka, Prometheus, Grafana

# C++ 编译 (WSL2)
cmake -B build -DCMAKE_BUILD_TYPE=Debug -DCMAKE_TOOLCHAIN_FILE=$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake
cmake --build build -j$(nproc)

# 启动单个GameNode (本地调试)
./build/game/game_node --config=configs/local.yaml
```

---

## 8. 代码风格规范

### 8.1 命名规范

| 类型 | 风格 | 示例 |
|------|------|------|
| 类/结构体 | PascalCase | `PlayerCharacter`, `AOIManager` |
| 函数/方法 | camelCase | `applyDamage()`, `getNearbyEntities()` |
| 变量 | snake_case | `player_id`, `max_hp` |
| 常量/宏 | UPPER_SNAKE | `MAX_SCENE_PLAYERS`, `DEFAULT_PORT` |
| 模块命名空间 | lowercase | `cami::character`, `cami::combat` |
| 文件名 | snake_case | `player_character.cpp`, `aoi_manager.h` |

### 8.2 代码示例

```cpp
// game/character/player_character.h
#pragma once

#include "game/event_bus/event.h"
#include "proto/flatbuffers/character_generated.h"

namespace cami::character {

// [PRODUCTION] 角色核心对象 - 内存中的单一权威源
class PlayerCharacter {
public:
    explicit PlayerCharacter(uint64_t player_id);
    ~PlayerCharacter();

    // === 查询接口 (返回const引用, 零开销) ===
    const CharacterStats& stats() const { return m_stats; }
    const Inventory& inventory() const { return m_inventory; }
    const BuffList& buffs() const { return m_buffs; }

    // === 修改接口 (含校验逻辑) ===
    // [EXPANSION_RISK] 高频调用, 需保证O(1)复杂度
    int32_t applyDamage(uint64_t source_id, int32_t raw_damage, DamageType type);
    bool applyBuff(uint32_t buff_id, uint64_t source_id, int32_t duration_ms);
    bool removeBuff(uint32_t buff_id);

    // === 持久化 ===
    void markDirty(DirtyFlag flag);
    bool serializeTo(flatbuffers::FlatBufferBuilder& builder) const;

private:
    uint64_t m_player_id;
    CharacterStats m_stats;
    Inventory m_inventory;
    BuffList m_buffs;
    uint32_t m_version;  // 乐观锁版本号
    DirtyFlags m_dirty_flags;

    // 内部校验
    int32_t calculateActualDamage(int32_t raw_damage, DamageType type) const;
};

} // namespace cami::character
```

### 8.3 注释规范

```cpp
// [PRODUCTION]      - 生产级实现, 已通过压测
// [PROTOTYPE]       - 原型实现, 待优化
// [EXPANSION_RISK]  - 扩容风险点, 标注瓶颈与上限
// [HOTFIX_BOUNDARY] - Lua热更边界, C++侧不可修改的部分
```

---

## 9. 测试策略

### 9.1 测试分层

| 层级 | 框架 | 位置 | 覆盖目标 |
|------|------|------|---------|
| 单元测试 | Google Test | tests/unit/ | 核心逻辑覆盖率 >= 80% |
| 集成测试 | Google Test + Docker | tests/integration/ | 模块间接口验证 |
| 压力测试 | 自研脚本 | scripts/benchmark/ | 5万并发验证 |
| 故障演练 | Chaos Mesh | scripts/chaos/ | 节点崩溃/网络分区 |
| 端到端测试 | 自研框架 | tests/e2e/ | 核心玩家流程 |

### 9.2 关键测试场景

- 1000人场景战斗循环耗时 <= 5ms
- 单节点消息吞吐 >= 20,000条/秒 [v3.0]
- GameNode崩溃后玩家800ms内迁移重连
- Redis分片故障时自动failover
- [v3.0] 负载70%触发YELLOW背压, 85%触发ORANGE, 100%触发RED
- [v3.0] L1缓存命中率 >= 95%
- [v3.0] 同节点IPC延迟 <= 0.05ms
- [v3.0] QUIC 0-RTT重连延迟 ~0ms
- [v3.0] WAL崩溃恢复零数据丢失
- [v3.0] 跨节点AOI消息量 <= 10条/s

---

## 10. 性能红线与容量规划

### 10.1 性能红线

| 指标 | 红线 | 检测方式 |
|------|------|---------|
| 战斗循环耗时(1000人) | <= 5ms | 每帧埋点 |
| 单节点消息处理 [v3.0] | <= 20,000条/s | Prometheus计数器 |
| Gateway↔GameNode延迟 [v3.0] | < 0.05ms | 共享内存计时 |
| 热数据读取延迟 [v3.0] | < 0.01ms (L1) | L1缓存计时 |
| 数据写入延迟 [v3.0] | < 0.1ms (WAL) | WAL写入计时 |
| 客户端重连延迟 [v3.0] | ~0ms (QUIC 0-RTT) | QUIC连接计时 |
| 连接迁移时间 | <= 800ms | 迁移流程计时 |
| Redis读延迟 | <= 1ms | Pipeline埋点 |
| gRPC调用延迟 | <= 2ms | gRPC metrics |
| 内存使用(单节点) [v3.0] | <= 10GB (含L1+SHM) | cgroup监控 |
| NavMesh寻路延迟 | <= 0.5ms | 寻路计时埋点 |
| LOS缓存命中率 | >= 90% | 缓存统计计数器 |
| LOS缓存查询延迟 | <= 0.001ms | 缓存查询计时 |
| 高频消息带宽(1000人) | <= 50KB/s | 网络流量监控 |
| 客户端预测正确率 | >= 95% | 纠偏频率统计 |
| 法术批次延迟(PVP) | <= 200ms | 批次窗口计时 |
| L1缓存命中率 [v3.0] | >= 95% | 缓存统计计数器 |
| CPU利用率 [v3.0] | >= 70% | 节点资源监控 |
| 跨节点AOI消息量 [v3.0] | <= 10条/s | 聚合器消息计数 |

### 10.2 容量规划 (5万在线) [v3.0 更新]

| 资源 | 数量 | 规格 | 说明 |
|------|------|------|------|
| 边缘网关 [v3.0] | 10+ | 2C4G | 就近接入, 降低RTT 80% |
| 中心网关 | 10+ | 4C8G | 每节点5000连接 |
| GameNode | 50+ | 8C16G | 每节点1000人, [v3.0] 吞吐20,000条/s |
| DataService [v3.0] | 4+ | 4C8G | **-50%** (L1缓存+Redis直连) |
| Redis分片 | 16 | 8C32G | 每分片~3000玩家热数据 |
| MySQL主库 | 8 | 8C32G+SSD | 每分库~6250玩家 |
| MySQL从库 [v3.0] | 8 | 4C16G+SSD | 读副本, 非关键读取 |
| 独立库(拍卖/公会) | 1组 | 4C16G | 全局共享数据 |
| Common Services | 各2+ | 2C4G | 每服务至少2副本 |
| 带宽 | 12Gbps | - | 含冗余 |
| Kafka | 3节点 | 4C8G | 高可用最少3节点 |
| 共享内存 [v3.0] | 50×1GB | - | Gateway↔GameNode IPC, 总50GB |
| WAL存储 [v3.0] | 50×50MB | - | 预写日志, 总2.5GB |

### 10.3 资源管控

- 怪物/Buff/技能实体全对象池预分配，禁止运行时动态申请
- 连接对象池化，复用内存
- Lua State 每场景一个实例，禁止每玩家独立State

---

## 11. 边界规则

### Always Do (始终遵守)
- 提交前运行测试
- 遵循命名规范
- 所有DB写入携带版本号
- 高频消息使用FlatBuffers
- 新增模块输出设计文档
- 标注 [PRODUCTION] / [PROTOTYPE] / [EXPANSION_RISK]

### Ask First (需先确认)
- 数据库Schema变更
- 新增第三方依赖
- 修改CI/CD配置
- 修改模块间接口
- 变更分片策略

### Never Do (禁止)
- 提交密钥/凭证
- GameNode直连MySQL
- 战斗循环内写DB
- JSON传高频战斗数据
- 无AOI全服广播
- 无版本号多节点并发写
- 模块间直接访问内部成员
- 全局锁/单点定时器
- 运行时动态申请实体对象

---

## 12. 成功标准

### 12.1 架构级
- [ ] 单集群支撑50,000并发在线
- [ ] 单GameNode支撑1,000人
- [ ] 50个GameNode线性扩容验证通过
- [ ] 无单点故障（任意节点崩溃不影响全服）

### 12.2 性能级
- [ ] 1000人场景战斗循环 <= 5ms
- [ ] 单节点消息吞吐 >= 20,000条/s [v3.0]
- [ ] 连接迁移 <= 800ms
- [ ] L1缓存命中率 >= 95% [v3.0]
- [ ] 同节点IPC延迟 <= 0.05ms [v3.0]
- [ ] 热数据读取延迟 <= 0.01ms (L1) [v3.0]
- [ ] 数据写入延迟 <= 0.1ms (WAL) [v3.0]
- [ ] QUIC 0-RTT重连延迟 ~0ms [v3.0]
- [ ] 跨节点AOI消息量 <= 10条/s [v3.0]
- [ ] 4级背压系统渐进式降级生效 [v3.0]

### 12.3 工程级
- [ ] 所有模块有独立设计文档
- [ ] 所有模块间接口有IDL定义
- [ ] 核心逻辑单元测试覆盖率 >= 80%
- [ ] 压测脚本可复现5万并发场景
- [ ] 故障演练手册完整

---

## 13. 待解决问题

| # | 问题 | 优先级 | 计划解决时间 |
|---|------|--------|------------|
| 1 | 跨服战场的事件总线具体实现方案 | P1 | Week 2 |
| 2 | Lua热更的状态迁移协议设计 | P1 | Week 2 |
| 3 | 拍卖行独立库的高可用方案 | P2 | Week 3 |
| 4 | 防外挂校验的具体检测规则 | P2 | Week 3 |
| 5 | 灰度发布的具体切流机制 | P2 | Week 4 |
| 6 | [v3.0] 共享内存IPC的崩溃恢复机制 | P1 | Week 3 |
| 7 | [v3.0] L1缓存一致性保证方案（多节点场景） | P1 | Week 4 |
| 8 | [v3.0] Cell边界AOI的平滑迁移策略 | P2 | Week 6 |
| 9 | [v3.0] QUIC在弱网环境下的丢包重传策略 | P2 | Week 5 |
| 10 | [v3.0] WAL文件的磁盘空间回收策略 | P3 | Week 5 |

---

## 附录 A: 架构决策记录 (ADR)

### ADR-001: 事件总线选型 — 进程内EventBus + Kafka

**决策**: GameNode内部模块间通信用进程内EventBus，异步场景用Kafka，跨服预留Redis Pub/Sub。

**理由**: 进程内EventBus零序列化开销，满足战斗循环5ms红线。Kafka适合异步持久化场景。跨服战场等场景延迟要求在5ms以内，Redis Pub/Sub足够。

**代价**: 跨GameNode事件需要额外中间件，增加架构复杂度。

### ADR-002: 模块边界 — WoW模式 (Player对象内存权威源)

**决策**: 角色模块拥有Player对象内存实例，其他模块通过const引用读取属性，通过方法调用修改属性。

**理由**: 战斗循环每帧需要读取大量角色属性，纯接口调用会引入不必要的开销。WoW模式在读取零开销的同时，通过方法封装保证写入校验。

**代价**: Player对象生命周期管理复杂，需要确保引用在对象销毁前释放。

### ADR-003: 数据分片 — PlayerID分片 + 独立库

**决策**: 玩家数据按PlayerID hash分片（Redis 16分片, MySQL 8分库），拍卖行/公会数据使用独立库。

**理由**: PlayerID分片保证同一玩家数据在同一分片，减少跨分片事务。拍卖行/公会需要全局查询，不适合按PlayerID分片。

**代价**: 独立库需要单独的高可用方案，增加运维复杂度。

### ADR-004: 技术选型 — vcpkg + Sol2 + 混合开发环境

**决策**: C++依赖管理用vcpkg，Lua-C++绑定用Sol2，本地开发用混合模式（WSL2编译 + Docker服务）。

**理由**: vcpkg与CMake集成最自然；Sol2是Lua 5.4最成熟的绑定库；混合模式兼顾编译性能和环境一致性。

### ADR-005: 客户端预测与服务器纠偏 [v2.0 新增]

**决策**: 采用客户端预测+服务器权威验证+纠偏的架构，客户端本地执行物理模拟，服务器验证并纠偏。延迟补偿解决高延迟命中判定问题。

**理由**: 无预测的MMO在网络延迟下体验极差。预测让操作"零延迟"感知，服务器纠偏保证数据一致性。

**代价**: 客户端需实现物理模拟逻辑，服务器需维护历史快照（~1秒数据，内存可控）。

### ADR-006: NavMesh寻路 + LOS缓存 [v2.0 新增]

**决策**: 使用NavMesh作为寻路基础设施，LOS检查结果缓存到空间哈希表（2米网格量化，5秒TTL）。

**理由**: NavMesh是3D空间寻路事实标准。LOS缓存将战斗循环中的视线检查从O(N)计算降到O(1)查表，命中率>95%。

**代价**: NavMesh数据需预生成和按场景加载（10-50MB/场景），LOS缓存需定期清理。

### ADR-007: AOI LOD三级更新 [v2.0 新增]

**决策**: 按距离分LOD0(30m/每帧)/LOD1(80m/100ms)/LOD2(200m/500ms)三级更新频率，配合属性压缩和视野锥裁剪。

**理由**: 全量每帧更新1000人消耗1.3MB/s带宽。LOD分级+属性压缩可将带宽降至47KB/s，节省97%。

**代价**: 远距离玩家位置更新有500ms延迟（视觉可接受），拥挤区域需动态调整LOD参数。

### ADR-008: 法术批次处理 [v2.0 新增]

**决策**: PVP场景启用100-200ms法术批次窗口，PVE场景关闭批次即时结算。

**理由**: 批次处理保证PVP公平性——双方同时对轰时同时结算。100ms在公平性和响应感间取得平衡。

**代价**: PVP法术结算有100-200ms延迟，需玩家可接受。

### ADR-009: 高频消息双轨协议 [v2.0 新增]

**决策**: 高频消息（位置/技能/AOI）使用自定义位压缩编码，复杂消息保留FlatBuffers。

**理由**: FlatBuffers消息体比专用协议大2-5倍。位压缩可将1000人带宽从1.3MB/s降至47KB/s。

**代价**: 需维护两套编解码器，位压缩协议的schema演进需手动处理。

### ADR-010: 场景Phasing/Layering [v2.0 新增]

**决策**: 引入Phase（任务进度驱动的世界状态分层）和Layer（平行层自动均衡）两种机制。

**理由**: Phase支持基于进度的动态世界变化，Layer解决热门区域人数过载问题。

**代价**: AOI需同时过滤Phase和Layer，增加查询复杂度。跨层交互需特殊处理。

### ADR-011: 共享内存IPC替代gRPC（同节点） [v3.0 新增]

**决策**: 同K8s节点的Gateway↔GameNode通信使用共享内存RingBuffer替代gRPC。

**理由**: gRPC的序列化+HTTP/2+TCP开销在5万并发下成为主要延迟来源（500+连接）。共享内存零拷贝、零序列化，延迟从1ms降至0.02ms。

**代价**: 要求Gateway和GameNode部署在同一K8s节点（亲和性约束），共享内存管理复杂（崩溃恢复、内存泄漏检测）。

### ADR-012: 场景级线程隔离 + Job System [v3.0 新增]

**决策**: GameNode从单线程主循环改为场景级线程隔离（每场景独立线程），非关键任务通过Job System并行化。

**理由**: 单线程下8核CPU利用率仅12.5%。场景级线程隔离让不同场景的AOI/战斗/寻路并行执行，CPU利用率提升至75%。

**代价**: 跨场景通信需要无锁队列，玩家跨场景迁移需要状态序列化/反序列化。

### ADR-013: 三级缓存 + Redis直连 + WAL [v3.0 新增]

**决策**: L1进程内缓存 → L2 Redis直连 → L3 MySQL。写入使用WAL保证持久性。

**理由**: 所有DB读写经DataService代理导致2跳延迟和单点瓶颈。L1缓存命中率>95%时几乎消除Redis往返。WAL替代30s批量flush实现零数据丢失。

**代价**: L1缓存需要内存（~2GB/节点），缓存一致性需要版本号校验。WAL需要额外磁盘空间。

### ADR-014: QUIC协议 + 边缘网关 [v3.0 新增]

**决策**: 客户端连接使用QUIC替代TCP，部署边缘网关就近接入。

**理由**: TCP 3次握手100ms首包延迟影响体验。QUIC 0-RTT消除重连延迟，连接迁移支持移动端网络切换。边缘网关降低玩家到服务器RTT 80%。

**代价**: QUIC实现复杂度高于TCP，边缘网关需要多地部署（增加运维成本）。

### ADR-015: 多级背压 + 自适应降级 [v3.0 新增]

**决策**: 4级背压（GREEN/YELLOW/ORANGE/RED）渐进式功能裁剪，替代85%硬性降级。

**理由**: 硬性降级在85%阈值处体验突变。4级渐进式降级让非关键功能逐步退出，核心体验尽可能保持。

**代价**: 降级策略配置复杂，需要精细的feature分级和恢复逻辑。

### ADR-016: Cell-based开放世界 [v3.0 新增]

**决策**: 大型开放世界场景分Cell管理（100m×100m），每Cell独立线程，支持Cell间迁移和负载均衡。

**理由**: 传统一个场景一个线程的模型在大场景(5000m+)下无法支撑高人数。Cell分片让1000人场景分散到25个Cell线程，每线程仅40人。

**代价**: Cell边界AOI需要额外管理，跨Cell迁移有开销（~5ms状态序列化）。

---

> **文档维护**: 本文档为活文档，架构决策变更时需同步更新并记录ADR。  
> **当前版本**: v3.0.0 | **ADR数量**: 16条 (ADR-001~016) | **详细设计**: 见 optimization-v2.md / optimization-v3.md
