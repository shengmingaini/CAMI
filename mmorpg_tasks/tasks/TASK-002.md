---
TASK-ID: TASK-002
NAME: Core Logger / Trace
PHASE: Phase 0 · 工程基础
MODULE: engine/core
OWNER: Codex / WorkBuddy Agent 实施；@技术总监 二次验收；本地 MinGW MSYS2 g++ + vcpkg 编译验证
STATUS: DONE
DEPENDENCIES: TASK-001
---

# TASK-002 · Core Logger / Trace

> 本文件由 `tools/gen/build_tasks.py` 从 `tools/gen/data_*.py` 生成，**禁止手工编辑**。
> 需要改动请修改数据源后重新生成：`python tools/gen/build_tasks.py`

| 字段 | 值 |
|---|---|
| TASK-ID | `TASK-002` |
| NAME | Core Logger / Trace |
| PHASE | Phase 0 · 工程基础 |
| MODULE | `engine/core` |
| OWNER | Codex / WorkBuddy Agent 实施；@技术总监 二次验收；本地 MinGW MSYS2 g++ + vcpkg 编译验证 |
| STATUS | **DONE** |
| DEPENDENCIES | `TASK-001` |

---

## 1. Objective

实现统一 Logger 与 TraceID 传播机制，使同一请求的全部日志可通过 TraceID 串起来；日志字段固定为 Timestamp/Level/Service/Module/TraceID/RequestID/PlayerID/SceneID/Message。

## 2. Dependencies

### 2.1 前置任务

- `TASK-001` · Core Error / Result 系统

### 2.2 门禁规则

验收脚本会先执行 `require_tasks_done 001`：
任一前置任务的 `STATUS` 不是 `DONE`，脚本立即非零退出，**禁止越级实施**。

## 3. Module

`engine/core`

## 4. State Owner（状态归属）

Logger 不持有任何业务状态。TraceID 由请求发起方（Gateway 入站或 Command 入口）生成并沿调用链透传，本任务只定义载体与传播规则。日志落盘由后台 IO 线程独占，业务线程只做无锁入队，禁止业务线程阻塞等 IO。

> 硬约束：同一实时状态只能有一个权威写入者（见 PROJECT_REQUIREMENTS §10 / §12）。
> 跨模块写入必须走 Command，禁止直接改对方内存。

## 5. Input

TASK-001 的 Result/Error（Logger 初始化失败用 Error 返回）；PROJECT_REQUIREMENTS.md 第 38 节

## 6. Output

Logger / LogContext / TraceID 实现 + 多线程压测 + TraceID 串联验证脚本

## 7. Public Interface

```cpp
namespace mmo::core {
enum class LogLevel : uint8_t { Trace=0, Debug=1, Info=2, Warn=3, Error=4, Fatal=5 };
struct LogContext {                    // 线程局部，随调用链自动传播
  TraceID   trace_id{0};
  RequestID request_id{0};
  PlayerID  player_id{kInvalidPlayerId};
  SceneID   scene_id{kInvalidSceneId};
  std::string_view module;             // 静态字符串，不允许运行时拼接
};
class ScopedLogContext final {         // RAII 覆盖/恢复，异常安全
 public:
  explicit ScopedLogContext(LogContext patch);
  ~ScopedLogContext();
};
class ILogSink { public: virtual ~ILogSink() = default;
  virtual void Write(const LogRecord&) noexcept = 0; };
class Logger final {
 public:
  static Result<void> Init(LoggerConfig cfg);
  static void RegisterSink(std::shared_ptr<ILogSink>);
  static void SetLevel(LogLevel);
  static bool ShouldLog(LogLevel) noexcept;
  static void Write(LogLevel, const LogContext&, std::string_view fmt, ...); // fmtlib 格式化
  static void Flush() noexcept;                 // 优雅退出时调用
};
}
#define MMO_LOG(level, fmt, ...)  do { if (::mmo::core::Logger::ShouldLog(level)) \
  ::mmo::core::Logger::Write(level, ::mmo::core::CurrentLogContext(), fmt, ##__VA_ARGS__); } while(0)
```

## 8. Data Model

**LogRecord（内部）**

| 字段 | 类型 | 说明 |
|---|---|---|
| timestamp_ns | int64 | 单调时钟 + 启动时刻换算出的墙钟，纳秒 |
| level | LogLevel | Trace..Fatal |
| service | string_view | gateway / gamenode / dataservice / control |
| module | string_view | 静态字符串，编译期常量 |
| trace_id | uint64 | 全链路唯一，跨进程透传 |
| request_id | uint64 | 单次 Command/Query 唯一 |
| player_id / scene_id | uint64 | 业务维度，无值填 kInvalid |
| thread_id | uint32 | 便于定位线程模型问题 |
| message | string | 格式化后文本 |

**TraceID**：`uint64`，由 `(node_id << 48) | (timestamp_low << 16) | counter` 生成，禁止用随机数（要可排序）。

## 9. Thread Model

Logger 为**无锁 MPSC 环形缓冲 + 独立后台刷盘线程**；业务线程只做格式化入队，不碰文件 IO。LogContext 存 thread_local，跨线程提交任务时必须显式拷贝传递（提供 `WithContext` 包装器）。

## 10. Hot Path

**NO** （但必须支持 `ShouldLog` 编译期/运行期短路，热路径关闭日志时开销 ≈ 单次分支）


## 11. External IO

**YES** （后台线程写文件，业务线程不阻塞）

所有外部 IO 必须异步化，禁止出现在 Tick 内。

## 12. Network RPC

**NO**


## 13. Persistence

**NO**


## 14. Files

- engine/core/include/mmo/core/log/
- engine/core/src/log/
- engine/core/tests/
- engine/core/docs/
- tools/logtrace/

## 15. Implementation Steps

1. 实现 log_level.h：LogLevel 与 ToString，支持从字符串配置（ConfigManager 后续接入，本任务先用 LoggerConfig 结构体）
2. 实现 trace_id.h：TraceID/RequestID 生成器（NodeID + 单调时钟 + 原子计数），提供 `NewTraceID()` / `DeriveRequestID(trace)`
3. 实现 log_context.h/.cpp：thread_local 当前上下文栈，ScopedLogContext RAII 覆盖恢复，提供 `WithContext(fn)` 用于跨线程携带
4. 实现 log_record.h：LogRecord POD，字段固定九项 + thread_id
5. 实现 async_ring_buffer.h：无锁 MPSC 环形队列（固定容量，满时按策略丢弃并计数，禁止阻塞业务线程）
6. 实现 logger.h/.cpp：Logger 静态门面 + ShouldLog 短路 + fmtlib 格式化（编译期校验格式串）
7. 实现两个 sink：ConsoleSink（带颜色，开发用）、RotatingFileSink（按大小滚动，保留 N 个，后台线程写）
8. 实现结构化 JSON 输出模式（配置开关），字段与 LogRecord 一一对应
9. 写 tools/logtrace/parse_trace.py：给定 TraceID，从日志文件中抽出全部相关行并按时间排序输出
10. 写测试：多线程（8 线程 × 每线程 10 万条）压测不丢不乱、字段完整、无交叉串行；TraceID 串联测试；关闭日志时开销测试（对比空循环）
11. 写 docs/README.md（字段说明 + 使用规范）与 docs/TEST.md

## 16. Unit Test

LogLevel 解析；TraceID 唯一性（100 万次无碰撞）与单调递增性；ScopedLogContext 嵌套恢复正确；环形队列单生产者单消费者顺序一致；格式化非法格式串时编译期报错

## 17. Integration Test

模拟一次跨线程请求：主线程设 TraceID → 投递到 WorkerThread（WithContext 携带）→ 子线程打日志 → 用 parse_trace.py 按 TraceID 检索，必须拿到全部 3 条日志且字段一致；RotatingFileSink 触发滚动后文件数量与命名正确

## 18. Benchmark

bin/log_bench：8 线程 × 100k 条，`log_ns_per_msg=`；关闭日志路径 `disabled_ns_per_call=`；丢包计数 `dropped=`

## 19. Failure Test

环形队列打满时：不阻塞、不崩溃、丢弃计数单调增加并产生一条 Warn；后台线程被卡住时业务线程仍能继续写入；磁盘不可写（指向只读目录）时 Logger::Init 返回 Error 而非崩溃；Fatal 级别写入后立即 Flush

## 20. Acceptance Criteria

1. 九项字段全部出现在每条日志记录中（JSON 模式下 key 齐全，单测断言）
2. 8 线程 × 10 万条压测无死锁、无交叉错乱，单线程内顺序严格递增
3. 给定 TraceID，`python tools/logtrace/parse_trace.py <trace>` 能完整串起跨线程日志
4. 关闭日志时单次 `MMO_LOG` 调用开销 < 5ns（benchmark 实测）
5. 环形队列满时 `dropped` 计数正确且业务线程不阻塞
6. 全仓 grep 无 `std::cout` / `printf` 直接输出（红线扫描）
7. Debug / Release 双构建通过，ctest -R Core_Log 全绿

以上每一条都必须在本地真实执行并留证；**任一条不满足即判定本任务未完成**，禁止进入下一个 TASK。

## 21. Forbidden

- 禁止在业务线程做文件 IO 或加锁写盘
- 禁止使用 std::cout / printf / std::cerr 直接输出
- 禁止用随机数生成 TraceID
- 禁止在热路径拼接字符串后再判断日志级别（必须先 ShouldLog）
- 禁止日志内容包含明文口令、令牌、完整身份证/银行卡等敏感数据
- 禁止日志队列满时阻塞业务线程

> 统一边界红线（全任务适用，详见 §27.3）：禁止扩散到他人 `module` 子树；下游禁止 `#include` 本任务 `src/`；禁止访问依赖模块内部数据；禁止在 `STATUS: DONE` 后静默改接口签名；禁止循环依赖。

## 22. Performance Expectation

开启日志：< 800ns/条（含格式化，8 线程并发）；关闭日志：< 5ns/次调用；单条日志堆分配次数 = 0（复用 per-thread scratch buffer）；丢弃率在 8×100k 压测下 < 0.1%。

## 23. Deliverables

- engine/core/include/mmo/core/log/logger.h
- engine/core/include/mmo/core/log/log_context.h
- engine/core/include/mmo/core/log/trace_id.h
- engine/core/src/log/*.cpp
- engine/core/tests/*
- tools/logtrace/parse_trace.py
- engine/core/docs/README.md
- engine/core/docs/PERFORMANCE.md

## 24. Verification Script（本地验收）

**验收脚本**：`scripts/verify/task-002.sh`（由生成器产出，禁止手工编辑）

```bash
# 默认 Release；可指定 Debug：BUILD_TYPE=Debug bash scripts/verify/task-002.sh
bash scripts/verify/task-002.sh
```

脚本执行的检查项：

1. 前置任务门禁：`require_tasks_done 001`
2. 交付物存在性检查（6 项）
3. 静态红线扫描：`engine/core/src` 内禁止出现 /\bstd::cout\s*<</
4. 静态红线扫描：`engine/core/src` 内禁止出现 /\bprintf\s*\(/
5. CMake configure + 编译（Debug + Release 双构建）
6. ctest 过滤执行：`-R Core_Log`
7. Benchmark 执行：`bin/log_bench --threads 8 --per-thread 100000`
8. 性能阈值断言：`bench/core_log.txt` 中 `disabled_ns_per_call` < `5`
9. 性能阈值断言：`bench/core_log.txt` 中 `log_ns_per_msg` ≤ `800`

脚本遵循 `set -euo pipefail`：任一步失败即非零退出，**不存在「警告通过」**。
脚本只报告真实执行结果，禁止兜底伪造 PASS；指标缺失直接判失败，禁止用估算值代替。

## 25. Git Commit

**必须先通过验收脚本（退出码 0），才允许提交。**

```bash
# 1) 把本任务标记为 DONE（脚本会校验 STATUS 流转合法）
bash scripts/task-done.sh TASK-002

# 2) 提交：Conventional Commits，scope 用模块名
git add -A
git commit -F - <<'EOF'
feat(core): Core Logger / Trace

- 实现要点：（填写本任务实际落地的内容，禁止复制 Objective）
- 实测数字：（粘贴 scripts/verify/task-002.sh 的真实输出，禁止写「性能良好」）

Refs: TASK-002
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
3. 查依赖：确认 TASK-001 均已 `STATUS: DONE`，否则停止并报告。
4. 查现状：grep 现有代码，确认要改的文件与符号真实存在，禁止凭空假设。
5. 守范围：只改本任务 §14 Files 范围内的文件；发现范围外问题只记录不修改。
6. 做实现：按 §15 Implementation Steps 顺序落地，每步可独立编译。
7. 本地编译：MinGW MSYS2 g++ + vcpkg manifest mode（baseline `aae277ac`），Debug 与 Release 都要过。
8. 跑单测：§16 Unit Test 全绿，新增代码必须带测试。
9. 跑集成：§17 Integration Test 全绿。
10. 跑 Benchmark：§18 真实执行，输出机器可读的 `key=value`，禁止估算。
11. 出结果：把实测数字写入 §22 对应的报告文件，不达标如实记录。
12. 跑验收脚本：`bash scripts/verify/task-002.sh` 退出码 0 后，才执行 §25 提交。

> **门禁**：本任务未通过 §20 Acceptance Criteria 与 §24 验收脚本，禁止进入下一个 TASK。

## 27. 接口契约、模块边界与扩展性

本节是**防任务间交付相互干扰 + 保框架可扩展**的统一契约，所有任务适用。

### 27.1 本任务导出的接口（冻结后不可破坏性变更）

见 §7 Public Interface。导出头只放在本任务 `include/` 下，签名一旦 `STATUS: DONE` 即视为契约冻结，下游依赖它；破坏性变更须走 `version` + 兼容性评估。

### 27.2 本任务消费的上游接口（来自前置任务，禁止绕过）

- `TASK-001` · `engine/core`：消费其 `include/` 下公开接口（详见该任务 §7 Public Interface），禁止 `#include` 其 `src/`

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
