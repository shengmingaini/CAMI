---
TASK-ID: TASK-001
NAME: Core Error / Result 系统
PHASE: Phase 0 · 工程基础
MODULE: engine/core
OWNER: Codex / WorkBuddy Agent 实施；@技术总监 二次验收；本地 MinGW MSYS2 g++ + vcpkg 编译验证
STATUS: DONE
DEPENDENCIES: TASK-000
---

# TASK-001 · Core Error / Result 系统

> 本文件由 `tools/gen/build_tasks.py` 从 `tools/gen/data_*.py` 生成，**禁止手工编辑**。
> 需要改动请修改数据源后重新生成：`python tools/gen/build_tasks.py`

| 字段 | 值 |
|---|---|
| TASK-ID | `TASK-001` |
| NAME | Core Error / Result 系统 |
| PHASE | Phase 0 · 工程基础 |
| MODULE | `engine/core` |
| OWNER | Codex / WorkBuddy Agent 实施；@技术总监 二次验收；本地 MinGW MSYS2 g++ + vcpkg 编译验证 |
| STATUS | **DONE** |
| DEPENDENCIES | `TASK-000` |

---

## 1. Objective

统一整个项目的错误处理：提供 Result<T> / ErrorCode / Error 三件套与 9 个标准错误码，服务端与客户端共用同一套定义，禁止任何模块自行设计冲突的错误体系。

## 2. Dependencies

### 2.1 前置任务

- `TASK-000` · 项目初始化与仓库规范

### 2.2 门禁规则

验收脚本会先执行 `require_tasks_done 000`：
任一前置任务的 `STATUS` 不是 `DONE`，脚本立即非零退出，**禁止越级实施**。

## 3. Module

`engine/core`

## 4. State Owner（状态归属）

无运行时状态。Error 与 Result<T> 为纯值类型（值语义、不可变、可跨线程自由传递），不存在 Owner 概念。错误码枚举表由本任务独占维护并集中定义，其他模块只能引用，禁止自定义第二套错误体系。失败路径必须零堆分配。

> 硬约束：同一实时状态只能有一个权威写入者（见 PROJECT_REQUIREMENTS §10 / §12）。
> 跨模块写入必须走 Command，禁止直接改对方内存。

## 5. Input

TASK-000 的空工程骨架；PROJECT_REQUIREMENTS.md 第 37 节「错误处理」

## 6. Output

engine/core 模块骨架 + error/result 头文件 + 单元测试 + 交付文档

## 7. Public Interface

```cpp
namespace mmo::core {
enum class ErrorCode : int16_t {
  OK = 0, INVALID_ARGUMENT = 1, NOT_FOUND = 2, TIMEOUT = 3, BUSY = 4,
  VERSION_CONFLICT = 5, UNAUTHORIZED = 6, RATE_LIMITED = 7, INTERNAL_ERROR = 8,
};
class Error final {                       // 值语义，无异常
 public:
  Error(ErrorCode c, std::string msg, std::string domain = "core");
  ErrorCode Code() const noexcept;
  std::string_view Message() const noexcept;
  std::string_view Domain() const noexcept;   // 产生错误的模块域，便于定位
  std::string ToString() const;               // "core/NOT_FOUND: player 42 not exist"
  bool IsRetryable() const noexcept;          // TIMEOUT / BUSY / RATE_LIMITED = true
};
template <typename T> class [[nodiscard]] Result {
 public:
  static Result Ok(T v); static Result Fail(Error e);
  bool HasValue() const noexcept; explicit operator bool() const noexcept;
  const T& Value() const&;                    // 无值时终止（断言 + 日志）
  const Error& Err() const&;
  T ValueOr(T fallback) const;
  // monadic：链式组合，避免层层 if
  template <typename F> auto AndThen(F&& f);
  template <typename F> auto Map(F&& f);
};
template <> class [[nodiscard]] Result<void>;   // 特化：只关心成功/失败
const char* ToString(ErrorCode) noexcept;       // 与数值双向映射，禁止重复定义
}
```

## 8. Data Model

| 类型 | 字段 | 说明 |
|---|---|---|
| ErrorCode | int16_t 枚举 | 9 个标准码，值固定，序列化后跨进程稳定 |
| Error | code / message / domain | 值语义，可拷贝，禁止抛异常 |
| Result&lt;T&gt; | variant&lt;T, Error&gt; | `[[nodiscard]]`，禁止丢弃返回值 |

## 9. Thread Model

全线程安全：`Error` 不可变，`Result<T>` 只读。禁止内部使用全局可变状态或静态缓存。

## 10. Hot Path

**NO** （但 Result 必须零异常、零动态分配失败路径，供热路径安全使用）


## 11. External IO

**NO**


## 12. Network RPC

**NO**


## 13. Persistence

**NO**


## 14. Files

- engine/core/include/mmo/core/error/
- engine/core/src/error/
- engine/core/tests/
- engine/core/docs/

## 15. Implementation Steps

1. 建模块目录 engine/core，按统一模板建 include/mmo/core、src、tests、docs、CMakeLists.txt
2. 实现 error_code.h：ErrorCode 枚举 + ToString/FromString 双向映射 + IsRetryable 判定
3. 实现 error.h/.cpp：Error 值类型，message 用 SSO 优化（≤32 字节不分配堆内存）
4. 实现 result.h：模板 Result<T> 与 Result<void> 特化，内部用 std::variant，加 `[[nodiscard]]`
5. 实现 AndThen/Map 链式操作，保证失败短路且不产生额外分配
6. 实现 `MMO_TRY(expr)` 宏（等价于 Rust 的 ?），失败时提前 return Err
7. 定义模块错误域常量（core / net / scene / combat / data / economy / lua），Error.domain 只允许取这些值
8. 写单元测试：每个错误码的 ToString/FromString 往返、Result 成功/失败路径、Value() 越界的断言行为、链式 AndThen 短路、Result<void> 语义、零分配路径验证（自定义 allocator 计数）
9. 写 docs/INTERFACE.md 与 docs/README.md，明确「禁止新增顶层错误码，扩展请用 domain 细分」
10. 在根 CMakeLists.txt 中 add_subdirectory(engine/core)，target 名 `mmo::core_error`

## 16. Unit Test

ErrorCode 往返映射全覆盖；Result 成功/失败/移动/拷贝语义；`[[nodiscard]]` 编译告警验证（丢弃返回值必须告警）；Result<void>；MMO_TRY 短路；失败路径零堆分配（allocator 计数器断言 alloc==0）

## 17. Integration Test

在一个假模块（tests/fixture）中用 Result 串联三层调用（repo→service→handler），验证错误码与 domain 原样透传不被改写；同一 Error 序列化后跨「进程边界模拟」反序列化一致

## 18. Benchmark

engine/core/tests/error_bench：1e7 次 Result 构造+析构、1e7 次失败路径传递；输出 `result_ns_per_op=` 与 `alloc_per_fail=`

## 19. Failure Test

错误码越界（FromString(999)）返回 nullopt 而非崩溃；Error message 超长（4KB）不截断溢出；Result 在移动后被访问时触发断言（Debug）而非 UB

## 20. Acceptance Criteria

1. 9 个错误码全部实现，ToString/FromString 双向一致（单测覆盖）
2. Result<T> 与 Result<void> 均带 `[[nodiscard]]`，丢弃返回值在 -Wall -Wextra 下产生告警
3. 单元测试全部通过（ctest -R Core_Error）
4. benchmark 输出 `result_ns_per_op` 且失败路径 `alloc_per_fail=0`
5. grep 全仓：除 engine/core 外**不存在**第二个 enum class ErrorCode 定义
6. Debug / Release 双构建通过
7. engine/core/docs/README.md 与 INTERFACE.md 存在且含使用示例

以上每一条都必须在本地真实执行并留证；**任一条不满足即判定本任务未完成**，禁止进入下一个 TASK。

## 21. Forbidden

- 禁止使用 C++ 异常作为业务错误传播机制（第三方库异常须在边界转换为 Error）
- 禁止任何模块自行定义新的顶层 ErrorCode 枚举
- 禁止在 Error 构造中做 IO、加锁或分配大对象
- 禁止 Result 失败路径产生堆分配
- 禁止用 int / bool 返回值代替 Result

> 统一边界红线（全任务适用，详见 §27.3）：禁止扩散到他人 `module` 子树；下游禁止 `#include` 本任务 `src/`；禁止访问依赖模块内部数据；禁止在 `STATUS: DONE` 后静默改接口签名；禁止循环依赖。

## 22. Performance Expectation

Result 构造+析构 < 5ns/op；失败路径堆分配次数 = 0；`ToString()` 单次 < 100ns（无格式化到 std::string 的额外拷贝）。数字由 benchmark 实测写入 docs/PERFORMANCE.md。

## 23. Deliverables

- engine/core/include/mmo/core/error/error_code.h
- engine/core/include/mmo/core/error/error.h
- engine/core/include/mmo/core/error/result.h
- engine/core/src/error/*.cpp
- engine/core/tests/*
- engine/core/docs/README.md
- engine/core/docs/INTERFACE.md
- engine/core/docs/PERFORMANCE.md

## 24. Verification Script（本地验收）

**验收脚本**：`scripts/verify/task-001.sh`（由生成器产出，禁止手工编辑）

```bash
# 默认 Release；可指定 Debug：BUILD_TYPE=Debug bash scripts/verify/task-001.sh
bash scripts/verify/task-001.sh
```

脚本执行的检查项：

1. 前置任务门禁：`require_tasks_done 000`
2. 交付物存在性检查（2 项）
3. 静态红线扫描：`engine/core/src/error` 内禁止出现 /\bthrow\s+/
4. 静态红线扫描：`engine/core/include/mmo/core/error` 内禁止出现 /\bthrow\s+/
5. CMake configure + 编译（Debug + Release 双构建）
6. ctest 过滤执行：`-R Core_Error`
7. Benchmark 执行：`bin/error_bench --iterations 10000000`
8. 性能阈值断言：`bench/core_error.txt` 中 `result_ns_per_op` ≤ `5`
9. 性能阈值断言：`bench/core_error.txt` 中 `alloc_per_fail` ≤ `0`

脚本遵循 `set -euo pipefail`：任一步失败即非零退出，**不存在「警告通过」**。
脚本只报告真实执行结果，禁止兜底伪造 PASS；指标缺失直接判失败，禁止用估算值代替。

## 25. Git Commit

**必须先通过验收脚本（退出码 0），才允许提交。**

```bash
# 1) 把本任务标记为 DONE（脚本会校验 STATUS 流转合法）
bash scripts/task-done.sh TASK-001

# 2) 提交：Conventional Commits，scope 用模块名
git add -A
git commit -F - <<'EOF'
feat(core): Core Error / Result 系统

- 实现要点：（填写本任务实际落地的内容，禁止复制 Objective）
- 实测数字：（粘贴 scripts/verify/task-001.sh 的真实输出，禁止写「性能良好」）

Refs: TASK-001
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
12. 跑验收脚本：`bash scripts/verify/task-001.sh` 退出码 0 后，才执行 §25 提交。

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
