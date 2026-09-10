---
TASK-ID: TASK-015
NAME: Movement System
PHASE: Phase 3 · GameNode Core
MODULE: server/gamenode/movement
OWNER: Codex / WorkBuddy Agent 实施；@技术总监 二次验收；本地 MinGW MSYS2 g++ + vcpkg 编译验证
STATUS: DONE
DEPENDENCIES: TASK-011, TASK-014
---

# TASK-015 · Movement System

> 本文件由 `tools/gen/build_tasks.py` 从 `tools/gen/data_*.py` 生成，**禁止手工编辑**。
> 需要改动请修改数据源后重新生成：`python tools/gen/build_tasks.py`

| 字段 | 值 |
|---|---|
| TASK-ID | `TASK-015` |
| NAME | Movement System |
| PHASE | Phase 3 · GameNode Core |
| MODULE | `server/gamenode/movement` |
| OWNER | Codex / WorkBuddy Agent 实施；@技术总监 二次验收；本地 MinGW MSYS2 g++ + vcpkg 编译验证 |
| STATUS | **DONE** |
| DEPENDENCIES | `TASK-011`, `TASK-014` |

---

## 1. Objective

实现移动系统：Move / Direction / Speed / Position / Velocity 与移动校验（防作弊）。**禁止数据库访问。**

## 2. Dependencies

### 2.1 前置任务

- `TASK-011` · Entity System
- `TASK-014` · AOI System（Dynamic Grid 第一版）

### 2.2 门禁规则

验收脚本会先执行 `require_tasks_done 011 014`：
任一前置任务的 `STATUS` 不是 `DONE`，脚本立即非零退出，**禁止越级实施**。

## 3. Module

`server/gamenode/movement`

## 4. State Owner（状态归属）

玩家坐标 (x, y, z) 与朝向的唯一写入者是所属 Scene 的 Movement System，且只能在 Movement 阶段写入。防加速 / 防穿墙校验必须在写入点完成，禁止绕过 Movement 直接改坐标。客户端上报的坐标是意图，不是权威。

> 硬约束：同一实时状态只能有一个权威写入者（见 PROJECT_REQUIREMENTS §10 / §12）。
> 跨模块写入必须走 Command，禁止直接改对方内存。

## 5. Input

TASK-011 MovementComponent；TASK-014 AOI（移动触发跨格与可见性更新）

## 6. Output

movement 模块 + 移动校验测试 + 反作弊测试

## 7. Public Interface

```cpp
namespace mmo::game::movement {
struct MovementState { Position pos; Vec3 velocity; float speed; float max_speed{6.0f};
                       uint32_t move_flags{0}; core::SteadyTime last_client_update; };
struct MoveCommand {                      // 客户端上行，必须可校验
  entity::EntityId entity; Position from; Position to; core::RequestID request_id;
  int64_t client_timestamp_ms; uint32_t client_seq; };
enum class MoveReject : uint8_t { None, TooFast, Teleport, OutOfBounds, NotMovable, RateLimited };
class MovementSystem { public:
  core::Result<void> ApplyCommand(const MoveCommand&, const scene::SceneContext&);
  core::Result<void> Integrate(const scene::SceneContext&, float dt_seconds);  // Tick 内积分
  core::Result<MoveReject> Validate(const MoveCommand&, const MovementState&) const noexcept;
  core::Result<void> SetSpeed(entity::EntityId, float) ;
  core::Result<void> Stop(entity::EntityId);
  MovementStats Stats() const noexcept;   // rejected_by_reason 分布，反作弊关键指标
};
}
```

## 8. Data Model

**校验规则（第一版）**

| 规则 | 判定 | 处理 |
|---|---|---|
| 速度上限 | 实际位移 / dt > max_speed × 1.15（15% 容差抗抖动） | 钳制到合法位置 + 计数 |
| 瞬移检测 | 单次位移 > max_speed × dt × 3 | 拒绝 + 回拉 + 计数 |
| 世界边界 | 超出 Scene 边界盒 | 钳制到边界 |
| 状态限制 | 眩晕/定身/死亡 | 拒绝（NotMovable） |
| 频率限制 | 上行包 > 30/s | 限流（RateLimited） |

所有拒绝必须**计数上报指标**（`rejected_by_reason`），便于识别作弊与误杀。

## 9. Thread Model

Movement 在 Scene 的 SimulationThread 的 Movement 阶段执行，无锁。客户端上行命令在 Input 阶段入队，Movement 阶段统一消费。

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

- server/gamenode/movement/include/mmo/game/movement/
- server/gamenode/movement/src/
- server/gamenode/movement/tests/
- server/gamenode/movement/benchmark/
- server/gamenode/movement/docs/

## 15. Implementation Steps

1. 定义 movement_state.h：MovementState 与 MoveCommand（含 client_seq 用于乱序/重放检测）
2. 实现 validator.h/.cpp：五条校验规则，返回 MoveReject 枚举，纯函数无副作用（便于单测）
3. 实现 movement_system.h/.cpp：ApplyCommand（校验 → 修正/拒绝 → 更新状态 → 触发 AOI Move）、Integrate（速度积分 + 地形高度采样占位）
4. 实现速度积分：v = dir × speed，pos += v × dt；支持加速度占位（第一版先匀速，接口预留）
5. 实现客户端位置纠偏：服务端权威位置与客户端上报偏差 > 阈值时下发位置纠正包（阈值可配，默认 0.5m）
6. 实现 AOI 联动：位置变化后调用 IAoi::Move 并把 entered/left 转 EventBus 事件
7. 实现移动统计：每 Tick 移动实体数、拒绝原因分布、纠偏次数
8. 写测试：五条校验规则各自的触发与不触发；边界情况（零位移、同帧多次移动、NaN、极大速度）；积分精度（匀速 10 秒位移误差 < 1cm）
9. 写反作弊测试：模拟 10 倍速移动、瞬移、高频发包，断言全部被拒绝且计数正确
10. 写 benchmark：1000 实体同时移动

## 16. Unit Test

五条校验规则单测全覆盖；积分精度；方向/速度计算；Stop/SetSpeed；client_seq 乱序与重放检测；拒绝原因统计计数

## 17. Integration Test

1000 实体在同一 Scene 随机移动 10000 Tick：位置始终合法（无越界、无超速）、AOI 可见集与参考实现一致、纠偏包数量在合理范围（< 5% Tick）；混合注入 10% 作弊包，全部被拒且不影响正常玩家

## 18. Benchmark

bin/movement_bench：`move_ns_per_entity=` / `validate_ns=` / `integrate_ns_per_1k=` / `aoi_update_ns=`

## 19. Failure Test

客户端上报 NaN：拒绝并保持原位置（禁止污染状态）；客户端上报极大坐标（1e30）：拒绝，不产生浮点溢出传播；客户端长时间不上报：按最后速度外推并在超时后 Stop；Entity 已销毁但仍有移动命令：返回 NOT_FOUND 不崩溃；AOI Move 失败：记录错误但位置更新不回滚（保证状态一致）

## 20. Acceptance Criteria

1. 五条校验规则全部实现，各有单测与反作弊集成测试
2. **Movement 代码中不存在任何数据库/Redis/网络调用**（红线扫描）
3. 匀速移动 10 秒位移误差 < 1cm（无累积漂移）
4. 1000 实体移动处理 < 600us/Tick（benchmark 实测，对应阶段预算）
5. NaN / 极大值 / 越界均被拒绝且不污染状态（单测）
6. 拒绝原因分布指标可采集（为反作弊运营提供数据）
7. Debug / Release 双构建通过，ctest -R Movement 全绿

以上每一条都必须在本地真实执行并留证；**任一条不满足即判定本任务未完成**，禁止进入下一个 TASK。

## 21. Forbidden

- 禁止在移动系统中访问数据库、Redis、gRPC、文件系统
- 禁止信任客户端上报的位置（必须服务端校验）
- 禁止直接接受客户端速度字段（速度由服务端属性决定）
- 禁止无容差的硬校验（会误杀正常玩家）
- 禁止让 NaN / Inf 进入位置状态
- 禁止在 Tick 内做阻塞式位置持久化

> 统一边界红线（全任务适用，详见 §27.3）：禁止扩散到他人 `module` 子树；下游禁止 `#include` 本任务 `src/`；禁止访问依赖模块内部数据；禁止在 `STATUS: DONE` 后静默改接口签名；禁止循环依赖。

## 22. Performance Expectation

单实体移动处理 < 500ns；1000 实体移动阶段 < 600us；校验 < 100ns/次；积分 < 50ns/实体；纠偏包带宽 < 总移动带宽的 5%。

## 23. Deliverables

- server/gamenode/movement/include/mmo/game/movement/movement_system.h
- server/gamenode/movement/include/mmo/game/movement/validator.h
- server/gamenode/movement/src/*.cpp
- server/gamenode/movement/tests/*
- server/gamenode/movement/benchmark/*
- server/gamenode/movement/docs/INTERFACE.md
- server/gamenode/movement/docs/PERFORMANCE.md

## 24. Verification Script（本地验收）

**验收脚本**：`scripts/verify/task-015.sh`（由生成器产出，禁止手工编辑）

```bash
# 默认 Release；可指定 Debug：BUILD_TYPE=Debug bash scripts/verify/task-015.sh
bash scripts/verify/task-015.sh
```

脚本执行的检查项：

1. 前置任务门禁：`require_tasks_done 011 014`
2. 交付物存在性检查（4 项）
3. 静态红线扫描：`server/gamenode/movement/src` 内禁止出现 /(mysql|redis|grpc|sql::|std::ifstream)/
4. CMake configure + 编译（Debug + Release 双构建）
5. ctest 过滤执行：`-R Movement`
6. Benchmark 执行：`bin/movement_bench --entities 1000 --ticks 10000`
7. 性能阈值断言：`bench/movement.txt` 中 `move_ns_per_entity` ≤ `500`

脚本遵循 `set -euo pipefail`：任一步失败即非零退出，**不存在「警告通过」**。
脚本只报告真实执行结果，禁止兜底伪造 PASS；指标缺失直接判失败，禁止用估算值代替。

## 25. Git Commit

**必须先通过验收脚本（退出码 0），才允许提交。**

```bash
# 1) 把本任务标记为 DONE（脚本会校验 STATUS 流转合法）
bash scripts/task-done.sh TASK-015

# 2) 提交：Conventional Commits，scope 用模块名
git add -A
git commit -F - <<'EOF'
feat(server): Movement System

- 实现要点：（填写本任务实际落地的内容，禁止复制 Objective）
- 实测数字：（粘贴 scripts/verify/task-015.sh 的真实输出，禁止写「性能良好」）

Refs: TASK-015
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
3. 查依赖：确认 TASK-011, TASK-014 均已 `STATUS: DONE`，否则停止并报告。
4. 查现状：grep 现有代码，确认要改的文件与符号真实存在，禁止凭空假设。
5. 守范围：只改本任务 §14 Files 范围内的文件；发现范围外问题只记录不修改。
6. 做实现：按 §15 Implementation Steps 顺序落地，每步可独立编译。
7. 本地编译：MinGW MSYS2 g++ + vcpkg manifest mode（baseline `aae277ac`），Debug 与 Release 都要过。
8. 跑单测：§16 Unit Test 全绿，新增代码必须带测试。
9. 跑集成：§17 Integration Test 全绿。
10. 跑 Benchmark：§18 真实执行，输出机器可读的 `key=value`，禁止估算。
11. 出结果：把实测数字写入 §22 对应的报告文件，不达标如实记录。
12. 跑验收脚本：`bash scripts/verify/task-015.sh` 退出码 0 后，才执行 §25 提交。

> **门禁**：本任务未通过 §20 Acceptance Criteria 与 §24 验收脚本，禁止进入下一个 TASK。

## 27. 接口契约、模块边界与扩展性

本节是**防任务间交付相互干扰 + 保框架可扩展**的统一契约，所有任务适用。

### 27.1 本任务导出的接口（冻结后不可破坏性变更）

见 §7 Public Interface。导出头只放在本任务 `include/` 下，签名一旦 `STATUS: DONE` 即视为契约冻结，下游依赖它；破坏性变更须走 `version` + 兼容性评估。

### 27.2 本任务消费的上游接口（来自前置任务，禁止绕过）

- `TASK-011` · `server/gamenode/entity`：消费其 `include/` 下公开接口（详见该任务 §7 Public Interface），禁止 `#include` 其 `src/`
- `TASK-014` · `server/gamenode/aoi`：消费其 `include/` 下公开接口（详见该任务 §7 Public Interface），禁止 `#include` 其 `src/`

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
