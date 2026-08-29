---
TASK-ID: TASK-014
NAME: AOI System（Dynamic Grid 第一版）
PHASE: Phase 3 · GameNode Core
MODULE: server/gamenode/aoi
OWNER: Codex / WorkBuddy Agent 实施；@技术总监 二次验收；本地 MinGW MSYS2 g++ + vcpkg 编译验证
STATUS: PENDING
DEPENDENCIES: TASK-011, TASK-012
---

# TASK-014 · AOI System（Dynamic Grid 第一版）

> 本文件由 `tools/gen/build_tasks.py` 从 `tools/gen/data_*.py` 生成，**禁止手工编辑**。
> 需要改动请修改数据源后重新生成：`python tools/gen/build_tasks.py`

| 字段 | 值 |
|---|---|
| TASK-ID | `TASK-014` |
| NAME | AOI System（Dynamic Grid 第一版） |
| PHASE | Phase 3 · GameNode Core |
| MODULE | `server/gamenode/aoi` |
| OWNER | Codex / WorkBuddy Agent 实施；@技术总监 二次验收；本地 MinGW MSYS2 g++ + vcpkg 编译验证 |
| STATUS | **PENDING** |
| DEPENDENCIES | `TASK-011`, `TASK-012` |

---

## 1. Objective

实现 AOI 系统，第一版用 Dynamic Grid 算法，提供 Enter / Leave / Move / QueryVisible / Broadcast 五个统一接口，并在 100 / 500 / 1000 / 2000 实体下测量查询延迟、广播成本与内存。

## 2. Dependencies

### 2.1 前置任务

- `TASK-011` · Entity System
- `TASK-012` · Scene System

### 2.2 门禁规则

验收脚本会先执行 `require_tasks_done 011 012`：
任一前置任务的 `STATUS` 不是 `DONE`，脚本立即非零退出，**禁止越级实施**。

## 3. Module

`server/gamenode/aoi`

## 4. State Owner（状态归属）

AOI 索引（格子 → 实体集合）的 Owner 是 AOI System，但索引内容只能在 Movement 阶段由 Scene 统一驱动更新，禁止其他时机写入。可见集（Visible Set）由 AOI 独占计算并缓存，其他模块只读；禁止在 AOI 之外维护第二份可见关系。

> 硬约束：同一实时状态只能有一个权威写入者（见 PROJECT_REQUIREMENTS §10 / §12）。
> 跨模块写入必须走 Command，禁止直接改对方内存。

## 5. Input

TASK-011 Entity/Position；TASK-012 Scene；PROJECT_REQUIREMENTS.md 第 16 节

## 6. Output

aoi 模块（Dynamic Grid）+ 四档规模基准 + 视野正确性测试

## 7. Public Interface

```cpp
namespace mmo::game::aoi {
struct AoiConfig { float cell_size{20.0f};      // 格子边长（米）
                   float view_radius{50.0f};    // 视距
                   size_t max_entities{10000};
                   bool  use_dynamic_grid{true}; };
class IAoi { public: virtual ~IAoi() = default;
  virtual core::Result<void> Enter(entity::EntityId, const Position&) = 0;
  virtual core::Result<void> Leave(entity::EntityId) = 0;
  virtual core::Result<MoveResult> Move(entity::EntityId, const Position& to) = 0;  // 返回进入/离开的观察者集合
  virtual core::Result<void> QueryVisible(entity::EntityId, std::vector<entity::EntityId>& out) const = 0;
  virtual core::Result<size_t> Broadcast(entity::EntityId, std::span<const uint8_t> payload) = 0;  // 只发给可见者
  virtual AoiStats Stats() const noexcept = 0; };
struct MoveResult { std::vector<entity::EntityId> entered; std::vector<entity::EntityId> left;
                    uint32_t touched_cells{0}; };
struct AoiStats { size_t entity_count; size_t cell_count; size_t avg_visible;
                  uint64_t last_query_ns; uint64_t last_broadcast_ns; size_t query_count; };
std::unique_ptr<IAoi> CreateDynamicGridAoi(AoiConfig);
}
```

## 8. Data Model

**Dynamic Grid**：格子边长 ≈ 视距的一半~等宽（默认 20m 格 / 50m 视距），查询时只扫描 3×3 邻域格子，禁止全 Scene O(N) 扫描。

**可见性规则**：距离 ≤ view_radius 且同 Scene 的实体互相可见；Enter/Leave 通过 MoveResult 的 entered/left 增量通知，禁止每帧全量重算可见集。

**事件**：EntityEnteredView / EntityLeftView，走 EventBus 异步派发。

## 9. Thread Model

AOI 只在所属 Scene 的 SimulationThread 的 AOI 阶段执行，无锁。跨格移动只更新涉及的格子（增量），禁止全表重建。

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

- server/gamenode/aoi/include/mmo/game/aoi/
- server/gamenode/aoi/src/
- server/gamenode/aoi/tests/
- server/gamenode/aoi/benchmark/
- server/gamenode/aoi/docs/

## 15. Implementation Steps

1. 实现 aoi.h：IAoi 接口与 AoiConfig / MoveResult / AoiStats
2. 实现 grid.h：格子索引结构（世界坐标 → cell key 的哈希映射，稀疏，禁止二维数组全覆盖导致内存爆炸）
3. 实现 dynamic_grid_aoi.h/.cpp：Enter（插入格子 + 计算初始可见集）/ Leave（移除 + 通知）/ Move（跨格检测 + 增量可见集 diff）
4. 实现 QueryVisible：3×3 邻域扫描 + 距离过滤，返回去重结果
5. 实现 Broadcast：遍历可见集发送，复用缓冲区（禁止每个目标单独分配/序列化）
6. 实现可见集 diff 算法：新旧可见集求交差，产生 entered/left，复杂度 O(k) 而非 O(N)
7. 实现内存优化：格子用 open-addressing 哈希 + 实体链表，避免每格 vector 造成碎片
8. 实现统计：avg_visible / 查询延迟 / 广播耗时 / 格子数量 / 每 Tick 跨格次数
9. 写正确性测试：随机 1000 实体随机游走 10000 步，与暴力 O(N²) 参考实现的可见集**逐一比对**必须一致
10. 写四档基准：100 / 500 / 1000 / 2000 实体，输出 query latency / broadcast cost / memory

## 16. Unit Test

Enter/Leave/Move 基本语义；跨格检测正确；QueryVisible 与暴力参考实现等价（1000 实体随机布局 100 次比对）；去重；边界（负坐标、超大坐标、同一格子多实体、实体重合）

## 17. Integration Test

在 Scene 中挂 1000 实体做随机游走 10000 Tick，与参考实现逐 Tick 比对可见集，差异率 = 0；广播场景下每个实体收到的消息数 = 其可见集大小（断言无重复无遗漏）

## 18. Benchmark

bin/aoi_bench：100/500/1000/2000 四档，输出 `entities=` / `query_ns_avg=` / `query_ns_p99=` / `broadcast_ns_per_target=` / `avg_visible=` / `mem_bytes_per_entity=` / `cross_cell_per_tick=`

## 19. Failure Test

实体超出世界边界：钳制或拒绝移动（写死一种并测试），禁止索引越界；格子数量为 0（空 Scene）：查询返回空集不崩溃；实体瞬间瞬移（跨 100 格）：正确产生一次大 diff 而非漏通知；实体数超 max_entities：返回 BUSY；坐标 NaN：拒绝移动并返回 INVALID_ARGUMENT

## 20. Acceptance Criteria

1. 五个接口（Enter/Leave/Move/QueryVisible/Broadcast）全部实现且有单测
2. **与暴力 O(N²) 参考实现的可见集 100% 一致**（1000 实体 × 100 次随机布局比对）
3. 100 / 500 / 1000 / 2000 四档基准数据全部产出并写入 docs/PERFORMANCE.md
4. 查询局部化：grep 确认不存在遍历全部实体的代码路径
5. 广播复用缓冲区，不为每个目标单独分配（benchmark 分配计数验证）
6. 坐标 NaN / 越界被拒绝，不产生越界访问
7. Debug / Release 双构建通过，ctest -R Aoi 全绿

以上每一条都必须在本地真实执行并留证；**任一条不满足即判定本任务未完成**，禁止进入下一个 TASK。

## 21. Forbidden

- 禁止 O(N) 全 Scene 扫描实现可见性查询
- 禁止每帧全量重算可见集（必须增量 diff）
- 禁止广播时对每个目标单独序列化/分配
- 禁止用全覆盖二维数组存稀疏世界（内存爆炸）
- 禁止在 AOI 内做数据库或网络访问
- 禁止允许 NaN / 越界坐标进入索引

> 统一边界红线（全任务适用，详见 §27.3）：禁止扩散到他人 `module` 子树；下游禁止 `#include` 本任务 `src/`；禁止访问依赖模块内部数据；禁止在 `STATUS: DONE` 后静默改接口签名；禁止循环依赖。

## 22. Performance Expectation

1000 实体：单次 QueryVisible < 5us、P99 < 15us；Move 处理 < 2us/实体；平均可见集 20~60；单实体 AOI 内存 < 128B；2000 实体下 AOI 阶段总耗时 < 600us。

## 23. Deliverables

- server/gamenode/aoi/include/mmo/game/aoi/aoi.h
- server/gamenode/aoi/include/mmo/game/aoi/dynamic_grid_aoi.h
- server/gamenode/aoi/src/*.cpp
- server/gamenode/aoi/tests/*
- server/gamenode/aoi/benchmark/*
- server/gamenode/aoi/docs/INTERFACE.md
- server/gamenode/aoi/docs/PERFORMANCE.md

## 24. Verification Script（本地验收）

**验收脚本**：`scripts/verify/task-014.sh`（由生成器产出，禁止手工编辑）

```bash
# 默认 Release；可指定 Debug：BUILD_TYPE=Debug bash scripts/verify/task-014.sh
bash scripts/verify/task-014.sh
```

脚本执行的检查项：

1. 前置任务门禁：`require_tasks_done 011 012`
2. 交付物存在性检查（2 项）
3. CMake configure + 编译（Debug + Release 双构建）
4. ctest 过滤执行：`-R Aoi`
5. Benchmark 执行：`bin/aoi_bench --entities 100,500,1000,2000 --ticks 10000`
6. 性能阈值断言：`bench/aoi_1000.txt` 中 `query_ns_p99` ≤ `15000`
7. 性能阈值断言：`bench/aoi_1000.txt` 中 `mem_bytes_per_entity` ≤ `128`

脚本遵循 `set -euo pipefail`：任一步失败即非零退出，**不存在「警告通过」**。
脚本只报告真实执行结果，禁止兜底伪造 PASS；指标缺失直接判失败，禁止用估算值代替。

## 25. Git Commit

**必须先通过验收脚本（退出码 0），才允许提交。**

```bash
# 1) 把本任务标记为 DONE（脚本会校验 STATUS 流转合法）
bash scripts/task-done.sh TASK-014

# 2) 提交：Conventional Commits，scope 用模块名
git add -A
git commit -F - <<'EOF'
feat(server): AOI System（Dynamic Grid 第一版）

- 实现要点：（填写本任务实际落地的内容，禁止复制 Objective）
- 实测数字：（粘贴 scripts/verify/task-014.sh 的真实输出，禁止写「性能良好」）

Refs: TASK-014
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
3. 查依赖：确认 TASK-011, TASK-012 均已 `STATUS: DONE`，否则停止并报告。
4. 查现状：grep 现有代码，确认要改的文件与符号真实存在，禁止凭空假设。
5. 守范围：只改本任务 §14 Files 范围内的文件；发现范围外问题只记录不修改。
6. 做实现：按 §15 Implementation Steps 顺序落地，每步可独立编译。
7. 本地编译：MinGW MSYS2 g++ + vcpkg manifest mode（baseline `aae277ac`），Debug 与 Release 都要过。
8. 跑单测：§16 Unit Test 全绿，新增代码必须带测试。
9. 跑集成：§17 Integration Test 全绿。
10. 跑 Benchmark：§18 真实执行，输出机器可读的 `key=value`，禁止估算。
11. 出结果：把实测数字写入 §22 对应的报告文件，不达标如实记录。
12. 跑验收脚本：`bash scripts/verify/task-014.sh` 退出码 0 后，才执行 §25 提交。

> **门禁**：本任务未通过 §20 Acceptance Criteria 与 §24 验收脚本，禁止进入下一个 TASK。

## 27. 接口契约、模块边界与扩展性

本节是**防任务间交付相互干扰 + 保框架可扩展**的统一契约，所有任务适用。

### 27.1 本任务导出的接口（冻结后不可破坏性变更）

见 §7 Public Interface。导出头只放在本任务 `include/` 下，签名一旦 `STATUS: DONE` 即视为契约冻结，下游依赖它；破坏性变更须走 `version` + 兼容性评估。

### 27.2 本任务消费的上游接口（来自前置任务，禁止绕过）

- `TASK-011` · `server/gamenode/entity`：消费其 `include/` 下公开接口（详见该任务 §7 Public Interface），禁止 `#include` 其 `src/`
- `TASK-012` · `server/gamenode/scene`：消费其 `include/` 下公开接口（详见该任务 §7 Public Interface），禁止 `#include` 其 `src/`

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
