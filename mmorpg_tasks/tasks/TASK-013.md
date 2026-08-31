---
TASK-ID: TASK-013
NAME: Simulation Scheduler（20Hz 固定 Tick）
PHASE: Phase 3 · GameNode Core
MODULE: server/gamenode/scheduler
OWNER: Codex / WorkBuddy Agent 实施；@技术总监 二次验收；本地 MinGW MSYS2 g++ + vcpkg 编译验证
STATUS: DONE
DEPENDENCIES: TASK-003, TASK-004, TASK-012
---

# TASK-013 · Simulation Scheduler（20Hz 固定 Tick）

> 本文件由 `tools/gen/build_tasks.py` 从 `tools/gen/data_*.py` 生成，**禁止手工编辑**。
> 需要改动请修改数据源后重新生成：`python tools/gen/build_tasks.py`

| 字段 | 值 |
|---|---|
| TASK-ID | `TASK-013` |
| NAME | Simulation Scheduler（20Hz 固定 Tick） |
| PHASE | Phase 3 · GameNode Core |
| MODULE | `server/gamenode/scheduler` |
| OWNER | Codex / WorkBuddy Agent 实施；@技术总监 二次验收；本地 MinGW MSYS2 g++ + vcpkg 编译验证 |
| STATUS | **DONE** |
| DEPENDENCIES | `TASK-003`, `TASK-004`, `TASK-012` |

---

## 1. Objective

实现 GameNode 固定 20Hz Tick 调度：Input → Movement → AOI → Combat → Buff → Quest → Event → Replication 八阶段顺序执行，且**每个阶段独立计时统计**。

## 2. Dependencies

### 2.1 前置任务

- `TASK-003` · Core Time / UUID / Config
- `TASK-004` · Core Memory / Thread / Scheduler
- `TASK-012` · Scene System

### 2.2 门禁规则

验收脚本会先执行 `require_tasks_done 003 004 012`：
任一前置任务的 `STATUS` 不是 `DONE`，脚本立即非零退出，**禁止越级实施**。

## 3. Module

`server/gamenode/scheduler`

## 4. State Owner（状态归属）

Scheduler 独占 TickNumber 与阶段推进权，只允许 Scene 的 Simulation 线程推进 Tick。Tick 内的八阶段（Input/Movement/AOI/Combat/Buff/Quest/Event/Replication）顺序固定，各系统的写权限由 Scene Owner 在该阶段内授权，同一状态不允许并行写入。Safe Point 只能由 Scheduler 宣告，其他模块不得自行判定。

> 硬约束：同一实时状态只能有一个权威写入者（见 PROJECT_REQUIREMENTS §10 / §12）。
> 跨模块写入必须走 Command，禁止直接改对方内存。

## 5. Input

TASK-003 TickClock（20Hz）；TASK-004 Scheduler/线程池；TASK-012 Scene 与 SceneContext

## 6. Output

scheduler 模块 + 阶段计时 + 长稳与抖动测试

## 7. Public Interface

```cpp
namespace mmo::game {
enum class TickPhase : uint8_t { Input, Movement, Aoi, Combat, Buff, Quest, Event, Replication };
constexpr const char* ToString(TickPhase) noexcept;
struct PhaseTiming {                       // 每阶段独立统计，禁止只报总 Tick
  TickPhase phase; uint64_t last_us; uint64_t avg_us; uint64_t p95_us; uint64_t p99_us; uint64_t max_us; };
class ISimulationStage { public: virtual ~ISimulationStage() = default;
  virtual TickPhase Phase() const noexcept = 0;
  virtual void Execute(const SceneContext&) = 0;      // 禁止在此阻塞
  virtual std::string_view Name() const noexcept = 0; };
class SimulationScheduler { public:
  struct Config { uint32_t hz{20}; uint32_t max_catchup{3}; DurationMs event_budget{2};
                  bool enable_phase_timing{true}; };
  core::Result<void> RegisterStage(std::unique_ptr<ISimulationStage>);   // 按 Phase 排序
  core::Result<void> Start(); void Stop() noexcept;
  core::Result<void> RunUntil(core::SteadyTime deadline);   // 测试用：手动驱动，不占线程
  const std::array<PhaseTiming, 8>& Timings() const noexcept;
  uint64_t TickNumber() const noexcept; uint64_t OverrunCount() const noexcept;  // 超 50ms 的 Tick 数
  double CpuUtilization() const noexcept; };
}
```

## 8. Data Model

**Tick 阶段与预算（1000 玩家 Scene 基准）**

| 阶段 | 预算 | 说明 |
|---|---|---|
| Input | 0.3ms | 消费上行命令队列 |
| Movement | 0.6ms | 位置积分与校验 |
| AOI | 0.6ms | 可见集计算 |
| Combat | 1.2ms | 技能/伤害结算 |
| Buff | 0.4ms | Buff tick 与过期 |
| Quest | 0.2ms | 事件驱动任务进度 |
| Event | 0.4ms | 事件派发（带预算） |
| Replication | 0.8ms | 状态打包下发 |
| **合计** | **< 5ms** | P95 目标 |

## 9. Thread Model

SimulationScheduler 每 Scene 一个实例，绑定**固定**一个 SimulationThread（线程亲和），禁止跨线程迁移。Stage 执行期间不得阻塞、不得做 IO。

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

- server/gamenode/scheduler/include/mmo/game/sched/
- server/gamenode/scheduler/src/
- server/gamenode/scheduler/tests/
- server/gamenode/scheduler/docs/

## 15. Implementation Steps

1. 定义 tick_phase.h：八阶段枚举与顺序常量（顺序写死，禁止运行期调整）
2. 定义 simulation_stage.h：ISimulationStage 接口
3. 实现 tick_timing.h：PhaseTiming 统计（用滑动窗口 + 分位数近似，禁止每次排序，用 HDR histogram 或固定桶）
4. 实现 simulation_scheduler.h/.cpp：基于 TASK-003 TickClock 的固定步长循环，含 CatchUp 限幅（max_catchup=3）
5. 实现阶段排序注册：按 TickPhase 排序，重复注册同 Phase 返回错误（防静默覆盖）
6. 实现帧 Arena：每 Tick 开始 Reset，结束后统一回收（配合 TASK-004 Arena）
7. 实现超时检测：单阶段超过预算时记录 warn 日志 + 指标，但不中断 Tick（第一版只观测不熔断）
8. 实现 Overrun 统计：Tick 总耗时 > 50ms 计数并记录最大耗时
9. 实现 RunUntil 手动驱动模式，供测试确定性驱动（禁止测试依赖 sleep）
10. 写测试：阶段顺序正确（用 mock stage 记录调用序）；每阶段计时独立且累加 ≈ 总耗时；CatchUp 限幅生效；Overrun 计数；帧 Arena 每帧 Reset
11. 写长稳测试：空 Scene 跑 10 分钟（12000 Tick），实测 Tick 数 = 12000 ± 5，无漂移

## 16. Unit Test

阶段注册排序与重复注册拒绝；TickClock 驱动精度；PhaseTiming 统计正确（注入已知耗时验证）；CatchUp 限幅；帧 Arena Reset 语义；Overrun 计数

## 17. Integration Test

挂 8 个 mock stage（每个注入固定耗时），跑 10000 Tick：断言阶段顺序严格为 Input→Movement→AOI→Combat→Buff→Quest→Event→Replication；断言每阶段统计值与注入值误差 < 5%；断言总耗时 ≈ 各阶段之和

## 18. Benchmark

bin/sched_sim_bench：`tick_overhead_ns=`（空 stage 开销）/ `timing_overhead_ns_per_phase=` / `drift_us_per_10min=`

## 19. Failure Test

单 stage 抛异常：捕获记录，跳过该阶段继续后续阶段，禁止整个 Tick 崩；某阶段严重超时：记录 warn + 指标，不阻塞后续；时钟跳变（手动注入大跨度 now）：CatchUp 被限幅到 3 步，不产生死亡螺旋；Stop 时正在执行的 Tick：完成当前 Tick 后停止，不中途杀

## 20. Acceptance Criteria

1. 八阶段按固定顺序执行（集成测试用 mock 记录调用序断言）
2. **每个阶段都有独立耗时统计**，且各阶段之和 ≈ 总 Tick 耗时（误差 < 5%）
3. 固定 20Hz：10 分钟长稳 Tick 数 = 12000 ± 5，无累积漂移
4. CatchUp 限幅生效（注入 5 秒空档，只补 3 个 Tick）
5. 单阶段异常不导致整个 Tick 崩溃（单测覆盖）
6. 重复注册同一 Phase 返回错误，不静默覆盖
7. Debug / Release 双构建通过，ctest -R Sched_Sim 全绿

以上每一条都必须在本地真实执行并留证；**任一条不满足即判定本任务未完成**，禁止进入下一个 TASK。

## 21. Forbidden

- 禁止只报告总 Tick 时间（必须分阶段）
- 禁止在 Tick 内做阻塞 IO（MySQL/Redis/gRPC/文件/网络）
- 禁止 Tick 频率可运行期随意调整（改频率需改配置并重走验收）
- 禁止死亡螺旋（必须 CatchUp 限幅）
- 禁止在 Tick 内大规模内存分配（走帧 Arena / 池）
- 禁止静默覆盖已注册的 Stage

> 统一边界红线（全任务适用，详见 §27.3）：禁止扩散到他人 `module` 子树；下游禁止 `#include` 本任务 `src/`；禁止访问依赖模块内部数据；禁止在 `STATUS: DONE` 后静默改接口签名；禁止循环依赖。

## 22. Performance Expectation

空 Scene 调度开销 < 20us/Tick；阶段计时自身开销 < 50ns/阶段；10 分钟漂移 < 50ms；Tick 抖动 P99 < 1ms（负载可控时）。

## 23. Deliverables

- server/gamenode/scheduler/include/mmo/game/sched/tick_phase.h
- server/gamenode/scheduler/include/mmo/game/sched/simulation_scheduler.h
- server/gamenode/scheduler/include/mmo/game/sched/tick_timing.h
- server/gamenode/scheduler/src/*.cpp
- server/gamenode/scheduler/tests/*
- server/gamenode/scheduler/docs/INTERFACE.md
- server/gamenode/scheduler/docs/PERFORMANCE.md

## 24. Verification Script（本地验收）

**验收脚本**：`scripts/verify/task-013.sh`（由生成器产出，禁止手工编辑）

```bash
# 默认 Release；可指定 Debug：BUILD_TYPE=Debug bash scripts/verify/task-013.sh
bash scripts/verify/task-013.sh
```

脚本执行的检查项：

1. 前置任务门禁：`require_tasks_done 003 004 012`
2. 交付物存在性检查（1 项）
3. CMake configure + 编译（Debug + Release 双构建）
4. ctest 过滤执行：`-R Sched_Sim`
5. Benchmark 执行：`bin/sched_sim_bench --ticks 12000`
6. 性能阈值断言：`bench/sched_sim.txt` 中 `tick_overhead_ns` ≤ `20000`
7. 性能阈值断言：`bench/sched_sim.txt` 中 `drift_us_per_10min` ≤ `50000`

脚本遵循 `set -euo pipefail`：任一步失败即非零退出，**不存在「警告通过」**。
脚本只报告真实执行结果，禁止兜底伪造 PASS；指标缺失直接判失败，禁止用估算值代替。

## 25. Git Commit

**必须先通过验收脚本（退出码 0），才允许提交。**

```bash
# 1) 把本任务标记为 DONE（脚本会校验 STATUS 流转合法）
bash scripts/task-done.sh TASK-013

# 2) 提交：Conventional Commits，scope 用模块名
git add -A
git commit -F - <<'EOF'
feat(server): Simulation Scheduler（20Hz 固定 Tick）

- 实现要点：（填写本任务实际落地的内容，禁止复制 Objective）
- 实测数字：（粘贴 scripts/verify/task-013.sh 的真实输出，禁止写「性能良好」）

Refs: TASK-013
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
3. 查依赖：确认 TASK-003, TASK-004, TASK-012 均已 `STATUS: DONE`，否则停止并报告。
4. 查现状：grep 现有代码，确认要改的文件与符号真实存在，禁止凭空假设。
5. 守范围：只改本任务 §14 Files 范围内的文件；发现范围外问题只记录不修改。
6. 做实现：按 §15 Implementation Steps 顺序落地，每步可独立编译。
7. 本地编译：MinGW MSYS2 g++ + vcpkg manifest mode（baseline `aae277ac`），Debug 与 Release 都要过。
8. 跑单测：§16 Unit Test 全绿，新增代码必须带测试。
9. 跑集成：§17 Integration Test 全绿。
10. 跑 Benchmark：§18 真实执行，输出机器可读的 `key=value`，禁止估算。
11. 出结果：把实测数字写入 §22 对应的报告文件，不达标如实记录。
12. 跑验收脚本：`bash scripts/verify/task-013.sh` 退出码 0 后，才执行 §25 提交。

> **门禁**：本任务未通过 §20 Acceptance Criteria 与 §24 验收脚本，禁止进入下一个 TASK。

## 27. 接口契约、模块边界与扩展性

本节是**防任务间交付相互干扰 + 保框架可扩展**的统一契约，所有任务适用。

### 27.1 本任务导出的接口（冻结后不可破坏性变更）

见 §7 Public Interface。导出头只放在本任务 `include/` 下，签名一旦 `STATUS: DONE` 即视为契约冻结，下游依赖它；破坏性变更须走 `version` + 兼容性评估。

### 27.2 本任务消费的上游接口（来自前置任务，禁止绕过）

- `TASK-003` · `engine/core`：消费其 `include/` 下公开接口（详见该任务 §7 Public Interface），禁止 `#include` 其 `src/`
- `TASK-004` · `engine/core`：消费其 `include/` 下公开接口（详见该任务 §7 Public Interface），禁止 `#include` 其 `src/`
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
