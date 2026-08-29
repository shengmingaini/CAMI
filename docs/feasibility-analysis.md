# 39 任务包 vs 项目目标 —— 可行性分析

日期：2026-08-29
对象：`F:\AI\workbuddy\CAMI\mmorpg_tasks\`（TASK-000 ~ TASK-038）
方法：逐份读取任务实际内容（Objective / State Owner / Implementation Steps / Acceptance），
     而非仅看标题；对照《Project Requirements V1.0 — Frozen Architecture》§1 的 10 条核心意图。

---

## 0. 结论（先行）

**能实现**：「服务器端基础框架 + 单集群内 1k–10k CCU 的可信工程验证」，并在 50k 水平上提供
「水平扩容验证路径」（TASK-038 七级 CCU 阶梯含 50k）。

**不能（在 39 任务内）完整实现**：一个**可玩的"类大型 MMORPG"**，缺口集中在社交玩法与运维控制面。
若你的真实目标是「可扩展的服务器框架 + 性能可证伪」（你原话是"服务器端基础框架"），当前包
**达标且工程质量高**；若要求完整社交玩法，则**不达标，需补任务**。

三个目标要素在 39 任务里没有落地：
1. **Social 运行时**（组队/好友/公会/聊天/邮件逻辑）—— 只有 DB 表，无实现任务
2. **ControlService 进程**（配置下发/灰度/版本/运维接口）—— 只被提及，无实现任务
3. **Live Scene Migration**（实时场景迁移）—— 被 TASK-037 明确推迟到第二阶段

一个被标记为"可接受风险"：
4. **Gateway 无多实例/HA** —— TASK-037 写明"Gateway 崩溃玩家无法重连（可接受，但需多实例）"，但多实例未实现

---

## 1. 项目目标逐项对照

| # | 目标要素 | 覆盖任务 | 判定 |
|---|---|---|---|
| 1 | 50k CCU 扩展路线 | TASK-038（阶梯含 50k=多节点扩容） | ✅ 验证路径有，控制面偏薄 |
| 2 | 开发阶段 100/500/1k/5k/10k | TASK-038 阶梯前 5 级 | ✅ |
| 3 | GameNode 无单点 | TASK-010 NodeRegistry + TASK-037 Failover | ✅ 重连式容灾（非实时迁移） |
| 4 | **Scene 可以独立迁移** | TASK-037（明确不做 Live Migration） | ⚠️ **推迟**，与目标 §1 张力 |
| 5 | 玩家可从故障 GameNode 重连 | TASK-037（六步重连 + Checkpoint 恢复） | ✅ |
| 6 | 战斗/移动热路径无同步阻塞 IO | TASK-013/014/015/022/023/024 + TASK-025 红线扫描 | ✅ |
| 7 | 不使用全局锁核心并发 | TASK-004（无全局锁）+ 静态扫描 | ✅ |
| 8 | 禁止 O(N) 全服广播 | TASK-014 AOI 局部查询 | ✅ 社交缺失时无全局广播需求 |
| 9 | 统一 Command/Query/Event | TASK-007 Bus | ✅ |
| 10 | 统一目录/风格/日志/错误/测试 | TASK-000 + TASK-001/002 + 39 份验收脚本 | ✅ 强项 |

> 10 条核心意图中 7 条完全覆盖、1 条推迟（Scene 迁移）、2 条非目标本身而是配套
> （Social/ControlService 是"功能/运维完整度"，不是 §1 列出的 10 条意图）。

---

## 2. 功能完整度（"类大型 MMORPG"视角）

| 子系统 | 覆盖任务 | 完整度 |
|---|---|---|
| Core（Error/Log/Time/Mem/Sched） | 001–004 | ✅ 完整 |
| 统一通信（Proto/gRPC/Bus/Transport/Session） | 005–009 | ✅ 完整 |
| Gateway（Router/路由/节点注册） | 010 | ✅ 含 NodeRegistry |
| GameNode Core（Entity/Scene/Scheduler/AOI/Movement） | 011–015 | ✅ 完整 |
| 基础玩法（Player/Inventory/NPC/Quest/World） | 016–020 | ✅ 完整 |
| 战斗（Skill/Damage/Buff/Combat） | 021–024 | ✅ 完整 |
| 战斗性能判定 | 025 | ✅ 架构可行性门禁 |
| 数据（DataService/Redis/MySQL） | 026–028 | ✅ 完整（8 逻辑分片不写死） |
| 经济 + 账本幂等 | 029–030 | ✅ 完整（五场景故障测试） |
| Lua（Runtime/HotReload/Gameplay） | 031–033 | ✅ 完整 |
| 客户端（Core/Renderer/Resource/LowSpec） | 034–036 | ⚠️ 验证用客户端，非可玩客户端 |
| 容灾（重连/Failover/Recovery） | 037 | ✅ 重连式（非实时迁移） |
| 压测/Chaos/交付 | 038 | ✅ 七级 CCU + Chaos |

### 明确缺口（功能视角）

| 缺口 | 现状 | 风险等级 |
|---|---|---|
| **Social 运行时** | TASK-028 建了 Guild/Mail 两张表，TASK-038 Bot 有 Chat 动作占位；但 Party/Friend/Guild/Chat/Mail 的**运行时逻辑无任务** | 🔴 高（若要"类大型 MMORPG"则必须补） |
| **ControlService 进程** | TASK-000 把四进程写进架构文档，TASK-003 说配置由它独占写入，TASK-010 说路由权威归它——但**没有任何任务实现它**（节点管理/配置下发/灰度/版本/运维接口） | 🟠 中（Scale-out 控制面依赖它；当前靠 NodeRegistry 部分替代） |
| **Live Scene Migration** | TASK-037 明确推迟 | 🟠 中（与目标 §1 张力，需规范层澄清） |
| **Gateway HA** | TASK-037 标注"需多实例"但无任务 | 🟡 低-中（可接受风险，第一版单 Gateway） |
| **拍卖行/跨服交易/邮件附件** | TASK-029 明确推迟 | 🟡 低（第一版可不要） |
| **可观测性（Prometheus/Grafana）** | 根规范 §40 即定为"后续" | ✅ 符合预期 |

---

## 3. 依赖 / 顺序风险

1. **战斗性能回归门禁偏弱**
   TASK-025（1k 战斗性能矩阵）是 TASK-022/023/024（战斗实现）的下游，顺序正确；
   但 TASK-033（Lua 玩法脚本，在 Combat Tick 内执行）在 TASK-025 之后才落地，
   Lua 脚本可能改变 Tick 耗时，而矩阵**不会在 Lua 之后重跑**，直到 TASK-038。
   → 建议：TASK-038 的 1k 级必须重跑完整战斗矩阵（或新增一个回归任务）。

2. **性能预算只报 1k，50k 靠 TASK-038 一次性验收**
   TASK-025 是 1k 判定点，TASK-038 才做 50k。中间 5k/10k 无独立性能门禁。
   → 可接受（TASK-038 逐级跑），但 50k 若不达标只能到最后才暴露，返工成本高。
   → 建议：TASK-038 内部把 1k/5k/10k 的 Tick 分解作为强制子门禁（已在阶梯表里，需强化断言）。

3. **跨进程端到端集成测试偏少**
   全链路只在 TASK-010（Login→Gateway→GameNode→Scene）和 TASK-037（故障演练）覆盖。
   DataService 与 GameNode 的端到端（TASK-026/027/028/029/030）以单测 + 组件集成测试为主，
   缺少一次"真实 gRPC 跨进程 + Redis + MySQL"的全链路演练。
   → 建议：在 TASK-038 之前补一个跨进程集成任务，或并入 TASK-030 的对账端到端。

---

## 4. 工程质量的真实强项（值得肯定）

- **每个任务都有 Unit / Integration / Benchmark / Failure / Acceptance / Forbidden** 六类规格
  + 一个可执行的 `scripts/verify/task-XXX.sh`，这是市面上罕见的高 rigor。
- **TASK-025 是真正的架构可行性判定点**（不达标禁止进 TASK-026），把"性能可证伪"落到实处。
- **State Ownership 在 39 份文件里 39 段专属描述**，直接回答"谁有权写"，与根规范 §10/§12 咬合。
- **红线扫描**（热路径禁 mysql/redis/grpc、禁 std::thread、禁裸 throw）把"不准做的事"变成了自动 CI 门禁。
- **STATUS 门禁**让"一个 TASK 未通过不许进下一个"有机械强制力。

---

## 5. 建议补充 / 澄清（按优先级）

| 优先级 | 动作 | 说明 |
|---|---|---|
| P0 | **澄清 Scene 迁移目标** | 规范 §1 与 §34 矛盾：要么把 §1 的"Scene 可以独立迁移"改为"长期目标，第一版不做"，要么立项 Phase 2 RFC。否则验收标准模糊。 |
| P0 | **决定 Social 是否第一版必须** | 若"类大型 MMORPG"要求社交，补 **TASK-039 Social 运行时**（Party/Friend/Guild/Chat/Mail，单 GameNode 内）；若只要框架，明确列为 Phase 2。 |
| P1 | **补 ControlService 最小实现**（或并入 TASK-010） | 至少实现：节点注册同步、配置版本下发、运维健康检查接口。当前 NodeRegistry 承担了部分，但 ControlService 的"配置/灰度/版本"职责悬空。 |
| P1 | **TASK-025 之后补一次战斗性能回归** | 在 TASK-033（Lua）完成后、TASK-038 之前，重跑 1k 战斗矩阵，防止 Lua 拖垮 Tick。 |
| P2 | **Gateway 多实例 / 前置于 Gateway 的 LB** | 若要真正"GameNode 无单点 + 入口不塌"，第一版至少把 Gateway 多实例 + 四层 LB 列入。 |
| P2 | **跨进程全链路集成任务** | 在 TASK-030 或 TASK-038 前，跑一次真实 gRPC+Redis+MySQL 端到端。 |

---

## 6. 一句话回答

> **39 个任务能实现你定义的核心目标（可扩展服务器框架 + 1k–50k 性能可证伪 + 重连式容灾）；
> 但"类大型 MMORPG"的社交玩法和运维控制面（Social / ControlService）缺任务，
> Live Scene 迁移被规范自身推迟。先把 P0 的"目标澄清 + Social 取舍"定下来，再开干，
> 避免做到一半发现方向对不上。**

---

## 7. 补充任务的依赖示意（若采纳）

```
TASK-039 Social 运行时
   deps: TASK-007 (Bus), TASK-011 (Entity), TASK-016 (Role), TASK-028 (MySQL: Guild/Mail 表)
   → Party/Friend 内存态在 GameNode；Guild/Mail 经 DataService 持久化

TASK-040 ControlService 最小实现
   deps: TASK-003 (Config), TASK-006 (gRPC), TASK-010 (NodeRegistry)
   → 节点注册同步、Config 版本下发、健康/运维接口；灰度发布留 Phase 2
```
