---
TASK-ID: TASK-003
NAME: Core Time / UUID / Config
PHASE: Phase 0 · 工程基础
MODULE: engine/core
OWNER: Codex / WorkBuddy Agent 实施；@技术总监 二次验收；本地 MinGW MSYS2 g++ + vcpkg 编译验证
STATUS: DONE
DEPENDENCIES: TASK-000
---

# TASK-003 · Core Time / UUID / Config

> 本文件由 `tools/gen/build_tasks.py` 从 `tools/gen/data_*.py` 生成，**禁止手工编辑**。
> 需要改动请修改数据源后重新生成：`python tools/gen/build_tasks.py`

| 字段 | 值 |
|---|---|
| TASK-ID | `TASK-003` |
| NAME | Core Time / UUID / Config |
| PHASE | Phase 0 · 工程基础 |
| MODULE | `engine/core` |
| OWNER | Codex / WorkBuddy Agent 实施；@技术总监 二次验收；本地 MinGW MSYS2 g++ + vcpkg 编译验证 |
| STATUS | **DONE** |
| DEPENDENCIES | `TASK-000` |

---

## 1. Objective

实现 Time（MonotonicClock / WallClock）、UUID 生成与 ConfigManager。**特别要求：游戏 Tick 计时只能基于 MonotonicClock，禁止使用系统墙钟作为唯一计时依据。**

## 2. Dependencies

### 2.1 前置任务

- `TASK-000` · 项目初始化与仓库规范

### 2.2 门禁规则

验收脚本会先执行 `require_tasks_done 000`：
任一前置任务的 `STATUS` 不是 `DONE`，脚本立即非零退出，**禁止越级实施**。

## 3. Module

`engine/core`

## 4. State Owner（状态归属）

配置快照（Config Snapshot）不可变，加载期由 ControlService 独占写入，运行期所有线程只读；版本切换通过原子指针整体替换，禁止原地修改。MonotonicClock 无状态，是 Tick 唯一允许的时间源。UUID 生成器按线程局部无锁实现。

> 硬约束：同一实时状态只能有一个权威写入者（见 PROJECT_REQUIREMENTS §10 / §12）。
> 跨模块写入必须走 Command，禁止直接改对方内存。

## 5. Input

TASK-000 工程骨架；PROJECT_REQUIREMENTS.md 第 13 节 Tick 模型

## 6. Output

time / uuid / config 三个子模块 + 单调时钟正确性测试 + 配置热加载测试

## 7. Public Interface

```cpp
namespace mmo::core {
// ---- 时钟 ----
class MonotonicClock {                       // 不受系统时间调整影响
 public:
  static SteadyNs   Now() noexcept;          // std::chrono::steady_clock 语义
  static SteadyTime Point() noexcept;
};
class WallClock {                            // 仅用于展示、日志时间戳、跨机对齐
 public:
  static int64_t UnixNanos() noexcept;
  static int64_t UnixMillis() noexcept;
};
// Tick 计时唯一入口：禁止业务直接调用 WallClock
class TickClock final {
 public:
  explicit TickClock(uint32_t hz) noexcept;  // 20Hz -> 50ms
  DurationMs TickInterval() const noexcept;
  SteadyTime NextTickDeadline(SteadyTime prev) const noexcept;
  uint32_t   CatchUpSteps(SteadyTime now, SteadyTime prev) const noexcept;  // 限幅，防死亡螺旋
};
// ---- UUID ----
class Uuid { public:
  static Uuid NewV4();                       // 随机，用于 MessageID / TransactionID
  static Uuid NewV7();                       // 时间有序，用于数据库主键
  std::string ToString() const; static Result<Uuid> Parse(std::string_view);
  std::array<uint8_t,16> bytes; };
// ---- Config ----
class ConfigManager { public:
  static Result<void> LoadFile(std::string_view path);   // JSON / YAML
  static Result<void> LoadDir(std::string_view dir);
  template <typename T> static Result<T> Get(std::string_view key);
  static Result<void> Set(std::string_view key, std::string value);  // 运行期覆盖，仅测试用
  static uint64_t Version() noexcept;                                 // 每次变更 +1
  static Result<void> Reload();                                       // 原子替换快照
};
}
```

## 8. Data Model

| 类型 | 表示 | 说明 |
|---|---|---|
| SteadyNs | int64 纳秒 | 单调时钟差值，永不受 NTP/手动改时间影响 |
| SteadyTime | steady_clock::time_point | Tick 调度基准 |
| Uuid | 16 字节 | V4（随机）/ V7（时间有序，DB 主键） |
| ConfigSnapshot | 不可变键值树 | 版本化，Reload 时原子替换，读者无锁 |

## 9. Thread Model

TickClock 只在 SimulationThread 使用，不加锁。ConfigManager 用 `shared_ptr<const Snapshot>` + 原子替换，读路径无锁；Reload 由 ControlService/运维线程触发。UUID 生成器用 thread_local 状态，**无全局锁**。

## 10. Hot Path

**YES** （TickClock 位于 Tick 循环入口，每次 Tick 调用一次）

本任务位于 Tick 热路径内，禁止：MySQL / Redis 同步访问 / 同步 gRPC / Kafka 同步访问 / 文件 IO / 网络阻塞 IO / 大规模内存分配（见 PROJECT_REQUIREMENTS §14）。

## 11. External IO

**YES** （ConfigManager 读文件，只在加载/热更时）

所有外部 IO 必须异步化，禁止出现在 Tick 内。

## 12. Network RPC

**NO**


## 13. Persistence

**NO**


## 14. Files

- engine/core/include/mmo/core/time/
- engine/core/include/mmo/core/uuid/
- engine/core/include/mmo/core/config/
- engine/core/src/{time,uuid,config}/
- engine/core/tests/
- config/

## 15. Implementation Steps

1. 实现 time/clock.h：MonotonicClock / WallClock 薄封装，明确注释「Tick 禁止用 WallClock」
2. 实现 time/tick_clock.h：固定 Hz（默认 20）、计算 deadline、CatchUpSteps 限幅（单帧最多补 3 个 Tick，防止死亡螺旋）
3. 实现 time/timer.h：基于单调时钟的一次性/周期定时器接口（为 TASK-004 Scheduler 预留，本任务只定义接口不实现调度）
4. 实现 uuid.h/.cpp：V4（OS 熵源）与 V7（时间前缀 + 随机），提供 ToString/Parse，禁止依赖 boost
5. 实现 config/config_manager.h/.cpp：JSON 解析（用已引入的第三方 JSON 库或自研极简解析），版本化快照、原子替换、Get<T> 模板特化
6. 实现配置校验：`Get` 缺失 key 返回 ErrorCode::NOT_FOUND 并带 key 名；类型不匹配返回 INVALID_ARGUMENT
7. 实现 `Reload()`：加载失败时保留旧快照并返回 Error，禁止半替换
8. 建立 config/ 目录：放 app.json（service 名、log 级别）、tick.json（hz=20）、network.json 占位
9. 写测试：单调性验证（在测试内无法改系统时间，改用注入时钟接口验证 WallClock 回拨时 TickClock 不受影响）；Tick 累计漂移测试（跑 10000 次理论 500s，误差 < 10ms）；UUID 唯一性 100 万次；配置热更原子性（Reload 期间并发读不崩溃且读到完整快照）
10. 写 docs/INTERFACE.md，明确「Tick 计时红线」

## 16. Unit Test

MonotonicClock 单调不回退（100 万次采样）；TickClock 20Hz 的 interval=50ms、CatchUpSteps 限幅生效；UUID V4/V7 格式正确、100 万次无碰撞、Parse/ToString 往返；Config Get/Set/Reload、错误码正确、快照版本递增

## 17. Integration Test

ConfigManager 在 Reload 过程中由 4 个读线程并发 Get，验证不会读到半更新状态；TickClock 驱动一个假 Tick 循环跑 10 秒，实测 Tick 次数 = 200 ± 2 且无累积漂移

## 18. Benchmark

bin/time_bench：`monotonic_ns_per_call=`（目标 < 25ns）、`uuid_v4_ns=` / `uuid_v7_ns=`（目标 < 100ns）、`config_get_ns=`（目标 < 50ns，读路径无锁）

## 19. Failure Test

配置文件损坏（非法 JSON）：LoadFile 返回 INVALID_ARGUMENT，旧快照保持不变；配置目录被删除：Reload 返回 NOT_FOUND 且服务继续用旧配置；注入时钟回拨 5 秒：TickClock 产生的 deadline 序列仍单调；UUID 熵源失败：返回 INTERNAL_ERROR 而非生成弱 UUID

## 20. Acceptance Criteria

1. 全仓 grep：Tick 相关代码路径中**不存在**对 WallClock / system_clock 的调用（红线扫描）
2. TickClock 跑 10000 次，累计误差 < 10ms，无漂移
3. UUID 100 万次无碰撞，V7 可按时间排序
4. ConfigManager 热更期间并发读安全（TSan 或压测 100 万次读无异常）
5. benchmark 三项指标达标
6. config/ 下至少 3 份配置可被正确加载并 Get 到值
7. Debug / Release 双构建通过，ctest -R Core_Time 全绿

以上每一条都必须在本地真实执行并留证；**任一条不满足即判定本任务未完成**，禁止进入下一个 TASK。

## 21. Forbidden

- 禁止用系统墙钟（std::chrono::system_clock / time(nullptr)）驱动游戏 Tick
- 禁止在 Tick 循环里读取配置文件
- 禁止 UUID 使用全局锁或共享随机引擎
- 禁止 ConfigManager 返回裸指针/引用给调用方长期持有
- 禁止配置加载失败时留下半更新状态

> 统一边界红线（全任务适用，详见 §27.3）：禁止扩散到他人 `module` 子树；下游禁止 `#include` 本任务 `src/`；禁止访问依赖模块内部数据；禁止在 `STATUS: DONE` 后静默改接口签名；禁止循环依赖。

## 22. Performance Expectation

MonotonicClock::Now < 25ns/次；UUID V4/V7 < 100ns/个；Config Get < 50ns/次（读路径无锁无分配）；TickClock 10 分钟累计漂移 < 50ms。

## 23. Deliverables

- engine/core/include/mmo/core/time/clock.h
- engine/core/include/mmo/core/time/tick_clock.h
- engine/core/include/mmo/core/uuid/uuid.h
- engine/core/include/mmo/core/config/config_manager.h
- engine/core/src/{time,uuid,config}/*.cpp
- engine/core/tests/*
- config/app.json
- config/tick.json
- config/network.json
- engine/core/docs/INTERFACE.md

## 24. Verification Script（本地验收）

**验收脚本**：`scripts/verify/task-003.sh`（由生成器产出，禁止手工编辑）

```bash
# 默认 Release；可指定 Debug：BUILD_TYPE=Debug bash scripts/verify/task-003.sh
bash scripts/verify/task-003.sh
```

脚本执行的检查项：

1. 前置任务门禁：`require_tasks_done 000`
2. 交付物存在性检查（8 项）
3. 静态红线扫描：`engine/core/src/time` 内禁止出现 /std::chrono::system_clock/
4. 静态红线扫描：`engine/core/include/mmo/core/time` 内禁止出现 /std::chrono::system_clock/
5. CMake configure + 编译（Debug + Release 双构建）
6. ctest 过滤执行：`-R Core_Time`
7. Benchmark 执行：`bin/time_bench --samples 1000000`
8. 性能阈值断言：`bench/core_time.txt` 中 `monotonic_ns_per_call` ≤ `25`
9. 性能阈值断言：`bench/core_time.txt` 中 `config_get_ns` ≤ `50`

脚本遵循 `set -euo pipefail`：任一步失败即非零退出，**不存在「警告通过」**。
脚本只报告真实执行结果，禁止兜底伪造 PASS；指标缺失直接判失败，禁止用估算值代替。

## 25. Git Commit

**必须先通过验收脚本（退出码 0），才允许提交。**

```bash
# 1) 把本任务标记为 DONE（脚本会校验 STATUS 流转合法）
bash scripts/task-done.sh TASK-003

# 2) 提交：Conventional Commits，scope 用模块名
git add -A
git commit -F - <<'EOF'
feat(core): Core Time / UUID / Config

- 实现要点：（填写本任务实际落地的内容，禁止复制 Objective）
- 实测数字：（粘贴 scripts/verify/task-003.sh 的真实输出，禁止写「性能良好」）

Refs: TASK-003
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
3. 查依赖：确认 TASK-000 均已 `STATUS: DONE`，否则停止并报告。
4. 查现状：grep 现有代码，确认要改的文件与符号真实存在，禁止凭空假设。
5. 守范围：只改本任务 §14 Files 范围内的文件；发现范围外问题只记录不修改。
6. 做实现：按 §15 Implementation Steps 顺序落地，每步可独立编译。
7. 本地编译：MinGW MSYS2 g++ + vcpkg manifest mode（baseline `aae277ac`），Debug 与 Release 都要过。
8. 跑单测：§16 Unit Test 全绿，新增代码必须带测试。
9. 跑集成：§17 Integration Test 全绿。
10. 跑 Benchmark：§18 真实执行，输出机器可读的 `key=value`，禁止估算。
11. 出结果：把实测数字写入 §22 对应的报告文件，不达标如实记录。
12. 跑验收脚本：`bash scripts/verify/task-003.sh` 退出码 0 后，才执行 §25 提交。

> **门禁**：本任务未通过 §20 Acceptance Criteria 与 §24 验收脚本，禁止进入下一个 TASK。

## 27. 接口契约、模块边界与扩展性

本节是**防任务间交付相互干扰 + 保框架可扩展**的统一契约，所有任务适用。

### 27.1 本任务导出的接口（冻结后不可破坏性变更）

见 §7 Public Interface。导出头只放在本任务 `include/` 下，签名一旦 `STATUS: DONE` 即视为契约冻结，下游依赖它；破坏性变更须走 `version` + 兼容性评估。

### 27.2 本任务消费的上游接口（来自前置任务，禁止绕过）

- `TASK-000` · `build / repo`：消费其 `include/` 下公开接口（详见该任务 §7 Public Interface），禁止 `#include` 其 `src/`

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
