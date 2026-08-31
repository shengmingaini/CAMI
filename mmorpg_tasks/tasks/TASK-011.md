---
TASK-ID: TASK-011
NAME: Entity System
PHASE: Phase 3 · GameNode Core
MODULE: server/gamenode/entity
OWNER: Codex / WorkBuddy Agent 实施；@技术总监 二次验收；本地 MinGW MSYS2 g++ + vcpkg 编译验证
STATUS: DONE
DEPENDENCIES: TASK-004, TASK-007
---

# TASK-011 · Entity System

> 本文件由 `tools/gen/build_tasks.py` 从 `tools/gen/data_*.py` 生成，**禁止手工编辑**。
> 需要改动请修改数据源后重新生成：`python tools/gen/build_tasks.py`

| 字段 | 值 |
|---|---|
| TASK-ID | `TASK-011` |
| NAME | Entity System |
| PHASE | Phase 3 · GameNode Core |
| MODULE | `server/gamenode/entity` |
| OWNER | Codex / WorkBuddy Agent 实施；@技术总监 二次验收；本地 MinGW MSYS2 g++ + vcpkg 编译验证 |
| STATUS | **DONE** |
| DEPENDENCIES | `TASK-004`, `TASK-007` |

---

## 1. Objective

实现统一实体系统：EntityID / EntityType / SceneID / Position / Components，支持 Create / Destroy / Find / AttachComponent / RemoveComponent。第一版采用 ECS-like Component 思想，不强制完整 ECS 框架。

## 2. Dependencies

### 2.1 前置任务

- `TASK-004` · Core Memory / Thread / Scheduler
- `TASK-007` · Command / Query / Event Bus

### 2.2 门禁规则

验收脚本会先执行 `require_tasks_done 004 007`：
任一前置任务的 `STATUS` 不是 `DONE`，脚本立即非零退出，**禁止越级实施**。

## 3. Module

`server/gamenode/entity`

## 4. State Owner（状态归属）

Entity 的生命周期（创建 / 销毁 / 组件挂载）由所属 Scene 在其 Simulation 线程上独占执行，禁止跨线程操作。EntityID 全局唯一且不可复用。组件数据的 Owner 是拥有该组件的系统（Movement 组件归 Movement、Combat 组件归 Combat），禁止跨系统直接写对方组件。

> 硬约束：同一实时状态只能有一个权威写入者（见 PROJECT_REQUIREMENTS §10 / §12）。
> 跨模块写入必须走 Command，禁止直接改对方内存。

## 5. Input

TASK-004 ObjectPool（实体对象池）；TASK-007 EventBus（实体生命周期事件）；TASK-005 协议中的 EntityId

## 6. Output

entity 模块 + 组件测试 + 批量创建销毁 benchmark

## 7. Public Interface

```cpp
namespace mmo::game {
using EntityId = uint64_t;                 // (index << 32) | generation，防 ABA
enum class EntityType : uint8_t { Player=1, Monster=2, Npc=3, Projectile=4, Effect=5, Item=6 };
struct Position { float x, y, z; float yaw; };
class IComponent { public: virtual ~IComponent() = default;
  virtual ComponentTypeId Type() const noexcept = 0;
  virtual void OnAttached(Entity&) {} virtual void OnDetached(Entity&) {} };
class Entity { public:
  EntityId Id() const noexcept; EntityType Type() const noexcept; SceneId Scene() const noexcept;
  const Position& Pos() const noexcept; void SetPos(const Position&) noexcept;
  template <typename C, typename... Args> C* AddComponent(Args&&...);
  template <typename C> C* TryGet() noexcept;
  template <typename C> const C* TryGet() const noexcept;
  template <typename C> bool RemoveComponent() noexcept;
  bool Alive() const noexcept; uint32_t Version() const noexcept; };
class EntityManager { public:
  Result<Entity*> Create(EntityType, SceneId, const Position&);
  Result<void> Destroy(EntityId);
  Entity* Find(EntityId) noexcept;              // O(1) slotmap 查找
  template <typename C> void Each(std::function<void(Entity&, C&)>);  // 组件遍历
  size_t Count(EntityType) const noexcept; size_t AliveCount() const noexcept; };
}
```

## 8. Data Model

**EntityId 编码**：`[generation:32][index:32]`，索引进 SlotMap 复用槽位、世代递增防 ABA。

**组件表**

| 组件 | 归属任务 | 说明 |
|---|---|---|
| MovementComponent | TASK-015 | 速度、朝向、移动目标 |
| CombatComponent | TASK-024 | HP/MP/战斗状态/目标 |
| BuffComponent | TASK-023 | Buff 列表与 tick 状态 |
| InventoryComponent | TASK-017 | 背包与装备引用 |
| AiComponent | TASK-018 | AI 状态机与仇恨目标 |

组件存储第一版用「每类型一个稀疏数组」而非每实体一个 map，遍历局部性更好。

## 9. Thread Model

Entity 的创建/销毁/组件变更**只在所属 Scene 的 SimulationThread 执行**（单 Owner）。跨线程只读快照通过 EntityId + version 校验。禁止共享可变 Entity。

## 10. Hot Path

**YES**

本任务位于 Tick 热路径内，禁止：MySQL / Redis 同步访问 / 同步 gRPC / Kafka 同步访问 / 文件 IO / 网络阻塞 IO / 大规模内存分配（见 PROJECT_REQUIREMENTS §14）。

## 11. External IO

**NO**


## 12. Network RPC

**NO**


## 13. Persistence

**NO**


## 14. Files

- server/gamenode/entity/include/mmo/game/entity/
- server/gamenode/entity/src/
- server/gamenode/entity/tests/
- server/gamenode/entity/benchmark/
- server/gamenode/entity/docs/

## 15. Implementation Steps

1. 定义 entity_id.h：EntityId 编解码（index/generation）、SlotMap 结构与 ABA 防护
2. 定义 entity.h：Entity 结构（id/type/scene/position/组件掩码/版本号），固定大小便于池化
3. 实现 component_store.h：每组件类型一个稀疏数组（dense array + sparse index），遍历连续、增删 O(1)
4. 实现 entity_manager.h/.cpp：Create（从 ObjectPool 取）/ Destroy（版本号 +1、槽位回收）/ Find（O(1)）/ Each（按组件遍历）
5. 实现 AddComponent / TryGet / RemoveComponent 模板接口，组件构造走 placement new + Arena/池
6. 实现生命周期事件：EntityCreated / EntityDestroyed / ComponentAttached / ComponentDetached（走 EventBus）
7. 实现延迟销毁：Destroy 标记为 pending，在当前 Tick 结束时统一回收（防止 Tick 中途悬垂指针）
8. 实现实体类型统计与指标：每类型存活数、创建/销毁速率、组件平均数量
9. 写测试：创建/查找/销毁；世代防 ABA（销毁后旧 Id 查找返回 nullptr）；组件增删查；遍历顺序与完整性；延迟销毁在 Tick 边界生效；1 万实体压力
10. 写 benchmark：10 万实体创建/销毁、组件遍历吞吐

## 16. Unit Test

EntityId 编解码往返；SlotMap 复用与世代递增；Create/Destroy/Find O(1)；组件增删查与类型安全（错误类型返回 nullptr）；延迟销毁语义；Each 遍历完整性

## 17. Integration Test

在 Scene 上下文（TASK-012 未就绪时用测试替身）创建 1 万实体（Player/Monster/Npc 混合），附加不同组件，跑 1000 个 Tick 的遍历与销毁，验证无泄漏、无悬垂（ASan）；销毁事件订阅者收到的顺序与内容正确

## 18. Benchmark

bin/entity_bench：`create_ns_per_entity=` / `destroy_ns_per_entity=` / `find_ns=` / `iterate_ns_per_1k=` / `mem_bytes_per_entity=`

## 19. Failure Test

重复 Destroy 同一 Id：幂等返回成功或 NOT_FOUND（二选一写死并测试），**禁止**重复回收导致槽位错乱；访问已销毁 Id：返回 nullptr 而非 UB；组件类型不匹配：返回 nullptr；实体数超上限：返回 BUSY 而非 OOM；Tick 中途销毁：不得产生悬垂指针（延迟销毁 + ASan 验证）

## 20. Acceptance Criteria

1. EntityId 含 generation，销毁后旧 Id 查找返回 nullptr（防 ABA，单测）
2. Create / Destroy / Find 均为 O(1)（benchmark 佐证：10 万实体下耗时线性且常数因子达标）
3. 延迟销毁在 Tick 边界统一执行，ASan 下无悬垂访问
4. 组件增删查类型安全，错误类型返回 nullptr
5. `mem_bytes_per_entity` 达标（目标见性能期望）
6. 全部生命周期事件可通过 EventBus 观测（集成测试订阅断言）
7. Debug / Release 双构建通过，ctest -R Entity 全绿

以上每一条都必须在本地真实执行并留证；**任一条不满足即判定本任务未完成**，禁止进入下一个 TASK。

## 21. Forbidden

- 禁止跨线程共享可变 Entity（单 Owner Simulation）
- 禁止在 Tick 中途立即回收实体（必须延迟到 Tick 边界）
- 禁止用 std::map/unordered_map 存组件导致遍历局部性差（第一版用稀疏数组）
- 禁止在无 generation 机制下复用实体槽位
- 禁止在实体系统中引入数据库或网络访问
- 禁止为每种实体类型写一套独立管理代码

> 统一边界红线（全任务适用，详见 §27.3）：禁止扩散到他人 `module` 子树；下游禁止 `#include` 本任务 `src/`；禁止访问依赖模块内部数据；禁止在 `STATUS: DONE` 后静默改接口签名；禁止循环依赖。

## 22. Performance Expectation

单实体内存 < 256B（不含组件）；Create < 100ns；Destroy < 80ns；Find < 20ns；遍历 1000 个实体 < 5us；10 万实体创建+销毁总耗时 < 100ms。

## 23. Deliverables

- server/gamenode/entity/include/mmo/game/entity/entity.h
- server/gamenode/entity/include/mmo/game/entity/entity_manager.h
- server/gamenode/entity/include/mmo/game/entity/component_store.h
- server/gamenode/entity/src/*.cpp
- server/gamenode/entity/tests/*
- server/gamenode/entity/benchmark/*
- server/gamenode/entity/docs/INTERFACE.md
- server/gamenode/entity/docs/PERFORMANCE.md

## 24. Verification Script（本地验收）

**验收脚本**：`scripts/verify/task-011.sh`（由生成器产出，禁止手工编辑）

```bash
# 默认 Release；可指定 Debug：BUILD_TYPE=Debug bash scripts/verify/task-011.sh
bash scripts/verify/task-011.sh
```

脚本执行的检查项：

1. 前置任务门禁：`require_tasks_done 004 007`
2. 交付物存在性检查（1 项）
3. CMake configure + 编译（Debug + Release 双构建）
4. ctest 过滤执行：`-R Entity`
5. Benchmark 执行：`bin/entity_bench --entities 100000`
6. 性能阈值断言：`bench/entity.txt` 中 `mem_bytes_per_entity` ≤ `256`
7. 性能阈值断言：`bench/entity.txt` 中 `create_ns_per_entity` ≤ `100`

脚本遵循 `set -euo pipefail`：任一步失败即非零退出，**不存在「警告通过」**。
脚本只报告真实执行结果，禁止兜底伪造 PASS；指标缺失直接判失败，禁止用估算值代替。

## 25. Git Commit

**必须先通过验收脚本（退出码 0），才允许提交。**

```bash
# 1) 把本任务标记为 DONE（脚本会校验 STATUS 流转合法）
bash scripts/task-done.sh TASK-011

# 2) 提交：Conventional Commits，scope 用模块名
git add -A
git commit -F - <<'EOF'
feat(server): Entity System

- 实现要点：（填写本任务实际落地的内容，禁止复制 Objective）
- 实测数字：（粘贴 scripts/verify/task-011.sh 的真实输出，禁止写「性能良好」）

Refs: TASK-011
EOF

# 3) 推送：GFW 屏蔽 ssh.github.com，必须走 22 端口
git push git@github.com:22:shengmingaini/CAMI.git main
```

提交规范：

- 类型：`feat` / `fix` / `perf` / `refactor` / `test` / `docs` / `build` / `chore`（本任务建议 `feat`）
- **一个 TASK 一次独立提交**，禁止把多个 TASK 合并提交
- 正文必须包含实测数字，禁止「性能良好 / 已优化」这类无法验证的描述
- 未通过验收脚本禁止提交，禁止 `--no-verify` 绕过

## 26. Codex Execution Rules

1. 读规范：先读 `PROJECT_REQUIREMENTS.md` 与本任务涉及章节，架构冻结，不得自行推翻。
2. 读任务：完整读完本文件全部章节再动手，禁止只看 Objective 就开始写。
3. 查依赖：确认 TASK-004, TASK-007 均已 `STATUS: DONE`，否则停止并报告。
4. 查现状：grep 现有代码，确认要改的文件与符号真实存在，禁止凭空假设。
5. 守范围：只改本任务 §14 Files 范围内的文件；发现范围外问题只记录不修改。
6. 做实现：按 §15 Implementation Steps 顺序落地，每步可独立编译。
7. 本地编译：MinGW MSYS2 g++ + vcpkg manifest mode（baseline `aae277ac`），Debug 与 Release 都要过。
8. 跑单测：§16 Unit Test 全绿，新增代码必须带测试。
9. 跑集成：§17 Integration Test 全绿。
10. 跑 Benchmark：§18 真实执行，输出机器可读的 `key=value`，禁止估算。
11. 出结果：把实测数字写入 §22 对应的报告文件，不达标如实记录。
12. 跑验收脚本：`bash scripts/verify/task-011.sh` 退出码 0 后，才执行 §25 提交。

> **门禁**：本任务未通过 §20 Acceptance Criteria 与 §24 验收脚本，禁止进入下一个 TASK。

## 27. 接口契约、模块边界与扩展性

本节是**防任务间交付相互干扰 + 保框架可扩展**的统一契约，所有任务适用。

### 27.1 本任务导出的接口（冻结后不可破坏性变更）

见 §7 Public Interface。导出头只放在本任务 `include/` 下，签名一旦 `STATUS: DONE` 即视为契约冻结，下游依赖它；破坏性变更须走 `version` + 兼容性评估。

### 27.2 本任务消费的上游接口（来自前置任务，禁止绕过）

- `TASK-004` · `engine/core`：消费其 `include/` 下公开接口（详见该任务 §7 Public Interface），禁止 `#include` 其 `src/`
- `TASK-007` · `engine/core`：消费其 `include/` 下公开接口（详见该任务 §7 Public Interface），禁止 `#include` 其 `src/`

### 27.3 模块边界红线（全任务统一）

- 模块 ≠ 进程：本任务代码只落在自身 `module` 子树（`include/` + `src/` + `tests/` + `docs/`），禁止扩散到其它任务拥有的目录。
- 下游只能通过本任务 `include/` 下的**公开头与接口**调用，禁止 `#include` 本任务 `src/` 或内部头（验收脚本会静态扫描本任务 `include/` 是否泄露内部 `src/`）。
- 本任务只调用依赖模块**声明**的接口，禁止访问其内部数据（如 `otherModule.internalData` 模式）。
- 接口在 `STATUS: DONE` 之后变更必须走 `version` 字段 + 兼容性评估，禁止静默改签名导致下游编译失败。
- 依赖方向单向（Game → Gameplay → Core），禁止循环依赖；新增模块不得破坏既有依赖环约束。

### 27.4 扩展性约束（可扩展框架兼容性）

- 新增同类能力（新 Command / 新 Event / 新 Scene 类型 / 新模块）必须走**注册表 / ID 段**机制，禁止在 `switch` 里硬编码穷举。
- 跨模块扩展点统一用抽象（C++ Interface / Command / Event），新增实现**不得修改既有任务文件**。
- 协议 / 接口变更必须带 `version` 字段并向下兼容，旧客户端 / 旧模块不得因此断连或编译失败。
- 所有模块遵循统一目录模板（include/src/tests/benchmark/docs/CMakeLists.txt）与五文档契约（README/INTERFACE/DEPENDENCY/PERFORMANCE/TEST），新增模块不得例外。

> 模块归属表（谁拥有哪棵子树）：本任务的 `module` 字段即其独占目录；
> 其它任务的 `module` 字段不得被本任务写入。统一模块模板见 `DEVELOPMENT.md` / 根规范 §6。

## 28. 变更记录

| 日期 | 变更 |
|---|---|
| 2026-08-29 | 方案 A 原地补齐：由 `tools/gen/build_tasks.py` 从结构化数据源重新生成，补齐 State Owner / 验收脚本 / STATUS 门禁 / Git Commit 规范 |
| 2026-08-29 | 完善：新增 §27 接口契约/模块边界/扩展性（全任务统一，防相互干扰）；新增 TASK-039 Social / TASK-040 ControlService / TASK-041 集成与回归；依赖相位自检跳过最终交付汇点；Scene Migration 登记为 Phase 2 RFC |
