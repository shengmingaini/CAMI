# mmorpg_tasks 任务包审查报告

审查对象：`F:\AI\workbuddy\CAMI\mmorpg_tasks\`（Codex 生成，40 个文件 / 39 份任务 + README）
审查时间：2026-08-29
审查方式：全量读取 + 结构化统计（章节唯一性、依赖图环检测、关键词覆盖度、目录一致性）

---

## 0. 结论（先说结果）

**这批文件不能直接用。它是一套「空壳模板」，不是任务规格。**

39 份文件里，只有 **4 个字段**真正携带任务信息，其余 **16 个章节（占 24 个章节的 67%）在 39 份文件中逐字节完全相同** —— 它们写的是「你要注意线程归属」这类通用守则，而不是「这个任务具体要做什么」。

直接把它丢给 Codex 执行，结果是：每个任务都会得到一份"正确的废话"，39 个任务产出的代码大概率互相接不上。

---

## 1. 核心证据

### 1.1 章节内容唯一性统计（39 份文件）

| 章节 | 唯一内容数 / 39 | 判定 |
|---|---:|---|
| Objective | **39** | 有信息量（每份一句话） |
| Dependencies | 34 | 有信息量 |
| Module | 31 | 有信息量 |
| Files | 31 | **伪差异**（见下） |
| State Owner | 1 | 全库雷同 |
| Input | 1 | 全库雷同 |
| Output | 1 | 全库雷同 |
| Public Interface | 1 | 全库雷同 |
| Data Model | 1 | 全库雷同 |
| Thread Model | 1 | 全库雷同 |
| Hot Path | 1 | 全库雷同 |
| External IO | 1 | 全库雷同 |
| Network RPC | 1 | 全库雷同 |
| Persistence | 1 | 全库雷同 |
| Implementation Steps | 1 | 全库雷同 |
| Unit Test | 1 | 全库雷同 |
| Integration Test | 1 | 全库雷同 |
| Benchmark | 1 | 全库雷同 |
| Failure Test | 1 | 全库雷同 |
| Acceptance Criteria | 1 | 全库雷同 |
| Forbidden | 1 | 全库雷同 |
| Performance Expectation | 1 | 全库雷同 |
| Deliverables | 1 | 全库雷同 |
| Codex Execution Rules | 1 | 全库雷同 |

> `Files` 的 31 个"唯一值"是假的：每份只有第一行不同（就是 Module 名），后 4 行（`protocol/`、`tests/`、`tools/`、`docs/`）全库相同。

### 1.2 举证：TASK-000 的 Implementation Steps（与其他 38 份一字不差）

```
1. 阅读 PROJECT_REQUIREMENTS.md、本任务以及所有 Dependencies。
2. 检查现有代码，不重复实现已有基础设施。
3. 先确定 Interface、State Ownership、Thread Ownership。
4. 编写最小可运行实现。
5. 添加单元测试。
...
```

这不是「项目初始化」的步骤，这是放之四海而皆准的十条通用动作。TASK-024（战斗框架整合）拿到的是**完全相同**的十条。

### 1.3 关键词覆盖度（全目录命中文件数）

| 关键词 | 命中 | 说明 |
|---|---:|---|
| MessageEnvelope | **0** | TASK-005 的核心交付物，但九字段一个没写 |
| FlatBuffers | **0** | 规范第 29 节要求的高频序列化，完全丢失 |
| Tick Safe Point | **0** | TASK-032 Lua 热更的硬约束，丢失 |
| shard | **0** | TASK-028 的 8 逻辑分片，丢失 |
| 50000 / 50,000 | **0** | 项目总目标 CCU，全包未出现 |
| 4-core | **0** | CLIENT LOW-SPEC 红线（4核/4GB/1GB VRAM），未落进 TASK-036 |
| ctest | **0** | 没有任何可执行测试命令 |
| verify / 脚本 | **0** | 没有任何验收脚本 |
| cmake | 1 | 仅出现在 TASK-000 标题 "Git/CMake/文档/CI" |
| 20Hz | 1 | 仅 TASK-013 Objective 一句话 |
| Dynamic Grid | 1 | 仅 TASK-014 Objective 一句话 |
| Redis / MySQL / Protobuf / gRPC / P95 / P99 / IdempotencyKey | 各 39 | **是模板文本，不是任务规格**（每份文件里那句通用守则各出现一次） |

---

## 2. 问题清单

### P0 — 阻塞使用，必须修

| # | 问题 | 证据 | 后果 |
|---|---|---|---|
| P0-1 | **16/24 章节是通用模板，无任务内容** | 见 1.1 | Codex 无法据此产出可对接的代码 |
| P0-2 | **依赖图存在环：TASK-012 ↔ TASK-013** | TASK-012 ← `TASK-011,TASK-013`；TASK-013 ← `TASK-004,TASK-012` | 死锁：两个任务都无法开始，串行执行链直接断掉 |
| P0-3 | **完全没有验收脚本** | 全目录 `verify/脚本/ctest` 命中 0 | 违反"本地编译和验证"要求；没有机器可判定的通过线 |
| P0-4 | **Acceptance Criteria 是通用八条** | 唯一内容数 = 1 | "编译通过、单测通过" —— 任何任务都能"通过"，形同虚设 |
| P0-5 | **无 Git commit 可执行规范** | 只有 README 第 11 行一句"并提交 Git commit" | 无 Conventional Commits 格式、无 commit body、无"验收不过禁止提交"门禁 |
| P0-6 | **Public Interface / Data Model 无实际定义** | 唯一内容数 = 1 | 39 个模块的接口未定义，跨模块对接必然返工 |

### P1 — 结构性问题，影响可实施性

| # | 问题 | 说明 |
|---|---|---|
| P1-1 | **目录命名与项目根规范冲突** | 规范根目录是 `engine/ server/ game/ client/ protocol/ database/ scripting/ tests/ tools/ docs/`；本包用了 `工程基座/`、`Core/`、`Game/`、`Data/Redis/`、`Data/MySQL/`、`Benchmark/`、`Server/Resilience/`、`Tools/QA/Delivery/` 等**规范中不存在的顶层目录** |
| P1-2 | **大小写与层级混用** | `Core/Network/`（大写）与 `protocol/`（小写）并存；`Data/Redis` 与 `Data/MySQL` 被拆成两个顶层，而规范里它们同属 DataService 进程内的 adapter |
| P1-3 | **违反统一模块模板** | 规范要求 `ModuleName/{include,src,tests,benchmark,docs,CMakeLists.txt}` + 五文档 `README/INTERFACE/DEPENDENCY/PERFORMANCE/TEST`；本包 `Files` 只给一个目录名 |
| P1-4 | **跨端依赖污染** | `TASK-034 Client ← TASK-008 Network`（服务端传输层）、`TASK-036 Client/Resources ← TASK-014 Game/AOI`（服务端 AOI）。规范明确 Server 与 Client 只通过 Protocol 契约衔接 |
| P1-5 | **层级倒置依赖** | `TASK-021 Skill ← TASK-020 Instance`（技能不该依赖副本）；`TASK-031 Lua ← TASK-021 Skill`（Lua 运行时不该依赖技能系统） |
| P1-6 | **TASK-025 依赖缺失** | 只依赖 `TASK-024/014/013`，**缺 TASK-015 Movement** —— 但它要跑 Movement 场景基准 |
| P1-7 | **TASK-025 无通过线** | Objective 只说"100/300/500/1000 玩家"，未写 5 场景 × 4 规模 = 20 组矩阵，也**未写 P95≤5ms / P99≤8ms**，更没有"不达标禁止进入 TASK-026"的门禁 |
| P1-8 | **TASK-037/038 未展开内部子项** | 原清单的 37.1 Node Registry / 37.2 Player Reconnect / 37.3 GameNode Failure / 37.4 Scene Recovery，以及 38.1 Bot / 38.2 Load Test / 38.3 Network Simulation / 38.4 Chaos / 38.5 Final Delivery，**全部丢失**，只剩一句 Objective |
| P1-9 | **无 STATUS 字段** | 任务文件里没有机器可读的 `STATUS: PENDING/DONE`，无法脚本化门禁"前置任务未 DONE 不得开工" |
| P1-10 | **CLIENT LOW-SPEC 红线未落地** | 4核/4GB/1GB VRAM 的目标与"以实测为准、不得预先宣称"的纪律，未进入 TASK-035/036 |

### P2 — 规范细节

| # | 问题 |
|---|---|
| P2-1 | 命名风格不符：你要的是 `task/task-000.md`，实际是 `mmorpg_tasks/tasks/TASK-000.md`（大写 + 复数目录） |
| P2-2 | 全部文件 CRLF 行尾（Windows 下无碍，但后续若加 `.sh` 脚本 + `.gitattributes eol=lf` 会冲突） |
| P2-3 | `TASK-000` 的 Files 写 `工程基座/` —— 一个规范里不存在的目录，而规范要求的 `README/LICENSE/PROJECT_REQUIREMENTS.md/ARCHITECTURE.md/DEVELOPMENT.md` 五份文件一个没提 |
| P2-4 | README 未说明本地验证环境（无 MinGW MSYS2、vcpkg manifest mode、BUILD_TYPE、ctest 等任何一条） |
| P2-5 | `State Owner` 字段写的是"若有新增运行时状态请写明"——这是给 AI 的提示语，不是本任务的状态归属结论 |

---

## 3. 与你三条硬要求的对照

| 你的要求 | 本包现状 | 判定 |
|---|---|---|
| 每份包含固定字段（TASK-ID/NAME/OBJECTIVE/DEPENDENCIES/MODULE/OWNER/INPUT/OUTPUT/PUBLIC INTERFACE/DATA MODEL/THREAD MODEL/HOT PATH/EXTERNAL IO/NETWORK RPC/PERSISTENCE/FILES/…） | 章节名**齐全**（24 个），但其中 16 个内容是模板；缺 `TASK-ID` 与 `OWNER` 两个字段（`OWNER` 被写成了 `State Owner`，语义被偷换） | ⚠️ 形似神不似 |
| 具体实现步骤 | 全库同一份十条通用动作 | ❌ |
| 验收脚本 | 0 个 | ❌ |
| 完成后 git commit 要求 | 一句话，无格式/无门禁/无示例 | ❌ |
| 编译和验证在本地完成 | 全包未提及本地工具链与验证路径 | ❌ |

---

## 4. 修复方案

### 方案 A：原地补齐 mmorpg_tasks（推荐）

保留现有 39 份的文件名与依赖骨架，逐份重写 16 个"雷同章节"为真实规格，并新增：

1. 修掉 `TASK-012 ↔ TASK-013` 环 —— 让 TASK-013（Scheduler）只依赖 TASK-004/003/012 的**接口**而非等 TASK-012 完成；建议改为 `TASK-012 ← TASK-011`，`TASK-013 ← TASK-003,TASK-004,TASK-012` 中把 012 去掉，由 TASK-013 自带 SceneContext 测试替身（我在规格里就是这么处理的）。
2. 修掉跨端与倒置依赖（P1-4 / P1-5）。
3. 补齐 TASK-025 依赖与通过线、TASK-037/038 内部子项、CLIENT LOW-SPEC 红线。
4. 目录改为规范路径：`engine/core/`、`server/gateway/`、`server/gamenode/<module>/`、`server/dataservice/`、`client/`、`benchmark/`、`tools/`。
5. 每份文件加 `STATUS: PENDING` 机器可读行。
6. 生成 `scripts/verify/task-XXX.sh` 共 39 个验收脚本 + `scripts/task-done.sh` 门禁。

### 方案 B：整包重做

用我在 `F:\AI\workbuddy\CAMI\tools\gen\` 已搭好的生成器重出一套（`task/task-000.md` ~ `task/task-038.md` + 39 个验收脚本）。该生成器已完成：验收框架 `scripts/verify/_common.sh`（含依赖 DONE 门禁、Debug/Release 双构建、ctest、benchmark 指标断言、红线扫描、端口检查）和模板渲染器 `build_tasks.py`；任务数据已填到 TASK-029（`data_a/b/c.py`），**TASK-030~038 的数据尚未填充**。

---

## 5. 建议

**采用方案 A**，理由：

- 现有包的依赖骨架（39 个节点、拓扑排序基本正确）是可用的，只是内容空——补内容比推倒重来省一半工作量。
- 文件名已与 Codex 上下文绑定，原地修不会丢失已建立的上下文。
- 生成器（方案 B）目前只填到 TASK-029，要补全还需同等量级的工作，且两套任务包并存会造成后续执行时的二义性。

需要我动手的话，告诉我走 A 还是 B，我直接开工。
