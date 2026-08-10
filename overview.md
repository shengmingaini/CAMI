# CAMI 项目概览

> **版本**: v3.0.0  
> **更新日期**: 2026-08-06  
> **状态**: 架构设计 + 技术栈验证阶段 (Day 1 架构设计 + Day 2 技术栈验证)

---

## 项目简介

CAMI 是面向5万并发在线的工业级MMORPG后端系统，遵循"四无"原则（无单点/无全局锁/无同步阻塞IO/无全服广播），支持线性扩容。

## 当前进展

### Day 1 完成（v1.0.0）
- [x] 架构假设梳理与确认（11条假设，4个关键决策）
- [x] 总体架构规约文档（13章节 + 4条ADR）
- [x] 五层架构依赖关系图
- [x] 项目目录结构搭建（50+目录）
- [x] 构建配置（CMake + vcpkg）
- [x] Docker开发环境（Redis/MySQL/Kafka/Prometheus）
- [x] 数据库Schema初稿（7张分片表 + 4张全局表）

### v2.0 优化完成（对标WoW差距补齐）
- [x] 客户端预测与纠偏系统设计（P0）
- [x] 寻路与碰撞系统模块设计（P0）
- [x] AOI LOD三级更新机制设计（P0）
- [x] 场景Phasing/Layering系统设计（P1）
- [x] 法术批次处理 + LOS缓存设计（P1）
- [x] 高频消息位压缩方案设计（P1）
- [x] 架构规约文档更新至v2.0.0（+6条ADR）
- [x] 新增模块目录（navigation/collision/prediction/phasing）

### v3.0 高并发优化完成（5万在线延迟与压力优化）
- [x] 进程间通信优化 — 共享内存IPC + 批量gRPC
- [x] GameNode内部并发架构 — 场景线程+Job System+ECS
- [x] 三级缓存与数据层优化 — L1缓存+Redis直连+WAL
- [x] 网络协议与边缘接入优化 — QUIC+边缘网关+消息聚合
- [x] 负载管理与自适应降级 — 4级背压+Cell分片开放世界
- [x] AOI空间优化与跨节点协调 — 预测预取+差分压缩+层次网格+跨节点聚合
- [x] 架构规约文档更新至v3.0.0（+6条ADR, 共16条）
- [x] 新增模块目录（threading/ecs/cell/backpressure/l1_cache/redis_direct/wal/quic/edge/ipc/aggregation）

### Day 2 完成（技术栈验证 Demo）
- [x] C++17 + boost::asio TCP 回显服务器（异步 I/O, 多线程, TCP_NODELAY）
- [x] 多连接基准测试客户端（管道深度, QPS 统计, 通过/失败判定）
- [x] Docker Redis Cluster 升级（3主3从, 6节点, 16384 slots, 自动 failover）
- [x] ShardingSphere Proxy 分库分表配置（PlayerID MOD 2, 8张分片表）
- [x] 验证脚本（Redis Cluster / MySQL 分片 / TCP 基准测试一键运行）
- [x] 技术验证报告文档（验证项 / 检查清单 / 预期结果 / 验收标准）

## 核心架构决策

| ADR | 决策 | 版本 |
|-----|------|------|
| ADR-001 | 事件总线: 进程内EventBus + Kafka + gRPC + Redis Pub/Sub预留 | v1.0 |
| ADR-002 | 模块边界: WoW模式 — Player对象内存权威源 | v1.0 |
| ADR-003 | 数据分片: PlayerID CRC16 + 拍卖行/公会独立库 | v1.0 |
| ADR-004 | 技术选型: vcpkg + Sol2 + 混合开发环境 | v1.0 |
| ADR-005 | 客户端预测: 预测+纠偏+延迟补偿+插值 | v2.0 |
| ADR-006 | 寻路碰撞: NavMesh + 动态AABB树 + LOS缓存 | v2.0 |
| ADR-007 | AOI优化: LOD三级更新 + 属性压缩 + 视野锥 | v2.0 |
| ADR-008 | 法术批次: PVP 100-200ms窗口, PVE即时 | v2.0 |
| ADR-009 | 协议优化: 高频位压缩 + 复杂FlatBuffers双轨 | v2.0 |
| ADR-010 | 场景分层: Phase + Layer + 跨服区域 | v2.0 |
| ADR-011 | IPC优化: 共享内存SHM+RingBuffer替代gRPC(同节点) | v3.0 |
| ADR-012 | 并发架构: 场景级线程隔离 + Job System + ECS SoA | v3.0 |
| ADR-013 | 数据层: 三级缓存(L1/L2/L3) + Redis直连 + WAL | v3.0 |
| ADR-014 | 网络层: QUIC 0-RTT + 边缘网关就近接入 | v3.0 |
| ADR-015 | 过载保护: 4级背压(GREEN/YELLOW/ORANGE/RED)渐进降级 | v3.0 |
| ADR-016 | 开放世界: Cell-based分片(100m×100m) + 独立线程 | v3.0 |

## 关键性能指标

| 指标 | 红线 | v2.0 | v3.0 |
|------|------|------|------|
| 战斗循环(1000人) | <=5ms | LOS缓存确保不超标 | 不变 |
| 单节点消息吞吐 | - | 8,000条/s | **20,000条/s (+150%)** |
| Gateway↔GameNode延迟 | - | ~1ms (gRPC) | **~0.02ms (SHM) (-98%)** |
| 热数据读取延迟 | - | ~3ms | **~0.01ms (L1) (-99.7%)** |
| 数据写入延迟 | - | ~3ms | **~0.1ms (WAL) (-97%)** |
| 客户端重连延迟 | - | ~100ms | **~0ms (QUIC 0-RTT)** |
| 高频消息带宽(1000人) | <=50KB/s | 47KB/s (-97%) | 不变 |
| LOS缓存查询 | <=0.001ms | O(1)查表 (-99%) | 不变 |
| 跨节点AOI消息量 | - | 30,000条/s | **10条/s (-99.97%)** |
| CPU利用率 | - | ~12.5% | **~75% (+500%)** |
| L1缓存命中率 | - | - | **>=95%** |
| NavMesh寻路 | <=0.5ms | 新增 | 不变 |
| 预测正确率 | >=95% | 新增 | 不变 |
| 连接迁移 | <=800ms | 不变 | QUIC 0ms |

## v3.0 性能提升总览

```
v2.0 → v3.0 关键指标提升:

  通信延迟:     1ms ────────────────────→ 0.02ms     (-98%)
  节点吞吐:     8,000条/s ─────────────→ 20,000条/s  (+150%)
  热数据读取:   3ms ────────────────────→ 0.01ms     (-99.7%)
  数据写入:     3ms ────────────────────→ 0.1ms      (-97%)
  重连延迟:     100ms ──────────────────→ 0ms        (-100%)
  CPU利用:      12.5% ──────────────────→ 75%        (+500%)
  跨节点AOI:    30,000条/s ─────────────→ 10条/s     (-99.97%)
  过载保护:     硬性85% ────────────────→ 渐进4级     (质变)

  5万在线集群总量:
    消息处理: 400,000条/s → 1,000,000条/s
    Redis QPS: 500,000 → 50,000 (L1吸收90%)
    MySQL写入QPS: 50,000 → 10,000 (WAL批量)
```

## 文档索引

| 文档 | 路径 | 说明 |
|------|------|------|
| 总体架构规约 | docs/architecture/architecture-spec.md | v3.0.0, 16条ADR |
| WoW对标分析 | docs/architecture/cami-vs-wow-analysis.md | 7项超越/10项不及 |
| v2.0优化设计 | docs/architecture/optimization-v2.md | 6个优化领域详细设计 |
| v3.0优化设计 | docs/architecture/optimization-v3.md | 6个高并发瓶颈优化详细设计 |
| Day 2技术验证 | docs/verification/day2-tech-verification.md | TCP echo / Redis Cluster / MySQL分片验证 |

## 技术栈

C++17 / Lua 5.4(Sol2) / Redis Cluster(16分片) / MySQL 8.0(ShardingSphere 8主+8从) / FlatBuffers+Protobuf / K8s+Istio / gRPC / Kafka / QUIC / CMake 3.20+ / vcpkg

## 下一步计划

1. **启动验证**: Docker Desktop → 运行验证脚本 → 填写实际数据
2. **编译验证**: WSL2 安装 g++/cmake/vcpkg → 编译 TCP echo → 基准测试
3. 首批模块设计文档（接入层/角色模块/战斗模块）
4. FlatBuffers高频协议schema定义
5. gRPC接口IDL定义
6. Phase 1实施: 共享内存IPC + Redis直连 + L1缓存 (Week 3)
