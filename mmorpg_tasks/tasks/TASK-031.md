---
TASK-ID: TASK-031
NAME: Lua Runtime
PHASE: Phase 7 · Lua 脚本
MODULE: scripting/lua
OWNER: Codex / WorkBuddy Agent 实施；@技术总监 二次验收；本地 MinGW MSYS2 g++ + vcpkg 编译验证
STATUS: PENDING
DEPENDENCIES: TASK-001, TASK-004, TASK-005, TASK-007, TASK-011
---

# TASK-031 · Lua Runtime

> 本文件由 `tools/gen/build_tasks.py` 从 `tools/gen/data_*.py` 生成，**禁止手工编辑**。
> 需要改动请修改数据源后重新生成：`python tools/gen/build_tasks.py`

| 字段 | 值 |
|---|---|
| TASK-ID | `TASK-031` |
| NAME | Lua Runtime |
| PHASE | Phase 7 · Lua 脚本 |
| MODULE | `scripting/lua` |
| OWNER | Codex / WorkBuddy Agent 实施；@技术总监 二次验收；本地 MinGW MSYS2 g++ + vcpkg 编译验证 |
| STATUS | **PENDING** |
| DEPENDENCIES | `TASK-001`, `TASK-004`, `TASK-005`, `TASK-007`, `TASK-011` |

---

## 1. Objective

实现 Lua 运行时：LuaVM / ScriptContext / Binding / Sandbox / Memory Limit / Execution Limit。C++ 向 Lua 暴露 Entity / Skill / Quest / Event / Query 能力。

## 2. Dependencies

### 2.1 前置任务

- `TASK-001` · Core Error / Result 系统
- `TASK-004` · Core Memory / Thread / Scheduler
- `TASK-005` · Protocol Schema（Protobuf + FlatBuffers）
- `TASK-007` · Command / Query / Event Bus
- `TASK-011` · Entity System

### 2.2 门禁规则

验收脚本会先执行 `require_tasks_done 001 004 005 007 011`：
任一前置任务的 `STATUS` 不是 `DONE`，脚本立即非零退出，**禁止越级实施**。

## 3. Module

`scripting/lua`

## 4. State Owner（状态归属）

脚本自身状态（全局变量）由 ScriptContext 拥有，归属 Scene 的 SimulationThread；脚本不得持有 C++ 对象指针跨 Tick（用 EntityId 句柄）。

> 硬约束：同一实时状态只能有一个权威写入者（见 PROJECT_REQUIREMENTS §10 / §12）。
> 跨模块写入必须走 Command，禁止直接改对方内存。

## 5. Input

TASK-011 Entity（绑定对象）；TASK-007 EventBus（脚本订阅事件）；TASK-005 协议；TASK-004 内存池（脚本分配）

## 6. Output

scripting/lua 模块 + 沙箱与限制测试 + 绑定测试

## 7. Public Interface

```cpp
namespace mmo::script {
struct LuaLimits { size_t memory_bytes{8*1024*1024};      // 单 VM 内存上限
                   uint32_t max_instructions{10'000'000};  // 单次调用指令上限（debug hook 计数）
                   DurationMs max_exec_time{5};            // 单次调用时间上限
                   uint32_t max_stack_depth{64};           // 防无限递归
                   bool allow_io{false};                   // 沙箱：禁 io/os 库
                   bool allow_loadstring{false}; };
class ScriptContext { public:                              // 每 Scene 一个，禁止全局单例
  static core::Result<std::unique_ptr<ScriptContext>> Create(LuaLimits);
  core::Result<ScriptId> Load(std::string_view name, std::string_view source);
  core::Result<void> Unload(ScriptId);
  template <typename... Args> core::Result<void> Call(ScriptId, std::string_view fn, Args&&...);
  core::Result<void> BindEntityApi(entity::EntityManager&);
  core::Result<void> BindEventApi(core::EventBus&);
  core::Result<void> BindQueryApi(core::QueryBus&);
  size_t MemoryUsed() const noexcept; uint32_t Version() const noexcept; };
enum class ScriptError : uint8_t { Ok, CompileError, RuntimeError, MemoryLimit,
                                   InstructionLimit, Timeout, StackOverflow, SandboxViolation };
core::Error ToCoreError(ScriptError) noexcept;
}
```

## 8. Data Model

**C++ → Lua 绑定面（白名单，禁止暴露整个引擎）**

| API | 方法 | 说明 |
|---|---|---|
| Entity | `entity.get(id)` / `entity.set_hp(id,v)` / `entity.get_pos(id)` | 只暴露只读与受控写 |
| Skill | `skill.cast(caster, skill_id, target)` | 走 SkillSystem 命令，禁止脚本直接改伤害 |
| Quest | `quest.set_progress(player, idx, v)` / `quest.complete(player, id)` | 走 QuestSystem |
| Event | `event.subscribe(name, fn)` / `event.publish(name, table)` | 经 EventBus，异步 |
| Query | `query.ask(name, table)` | 只读取，禁止副作用 |

**沙箱**：移除 `io` / `os` / `loadstring` / `require` / `debug`（除 hook）；只保留 `string` / `table` / `math` 的安全子集。
**禁止**：脚本直接访问 Entity 内部内存指针、直接改 HP 数值、做文件/网络 IO。

## 9. Thread Model

Lua VM **每 Scene 一个**，只在所属 Scene 的 SimulationThread 执行，**禁止跨线程共享 VM**。脚本调用为同步，受指令数与时间双重上限约束。

## 10. Hot Path

**YES** （技能公式、Buff 公式会逐次调用）

本任务位于 Tick 热路径内，禁止：MySQL / Redis 同步访问 / 同步 gRPC / Kafka 同步访问 / 文件 IO / 网络阻塞 IO / 大规模内存分配（见 PROJECT_REQUIREMENTS §14）。

## 11. External IO

**NO** （沙箱禁 IO）


## 12. Network RPC

**NO**


## 13. Persistence

**NO**


## 14. Files

- scripting/lua/include/mmo/script/
- scripting/lua/src/
- scripting/lua/tests/
- scripting/lua/benchmark/
- scripting/lua/docs/

## 15. Implementation Steps

1. 引入 Lua（vcpkg lua 或 luajit，锁定版本），接入 CMake，产物不入库
2. 实现 lua_vm.h/.cpp：VM 创建与销毁，绑定自定义 allocator（走 TASK-004 MemoryPool，便于限额与统计）
3. 实现 script_context.h/.cpp：Load/Unload/Call，每 Scene 一个实例
4. 实现内存限制：自定义 allocator 计数，超限抛 MemoryLimit 并回滚到安全点
5. 实现指令与时间上限：用 lua_sethook(L, LUA_MASKCOUNT) 计数 + 单调时钟检查，双保险
6. 实现栈深限制：hook 中检查栈深度，防无限递归导致 C 栈溢出
7. 实现沙箱：移除 io/os/loadstring/require/debug，只保留安全子集（白名单而非黑名单）
8. 实现五项绑定：Entity / Skill / Quest / Event / Query，全部走 C++ 系统接口（禁止直改内部）
9. 实现错误映射：ScriptError → mmo::core::Error（TASK-001），含行号与脚本名
10. 写测试：沙箱逃逸尝试（调 io.open 应失败）、内存超限、指令超限、超时、栈溢出、脚本语法错误、脚本运行时错误（不崩溃宿主）、绑定 API 正确性
11. 写 benchmark：脚本调用开销（目标 < 2us/次）

## 16. Unit Test

VM 创建销毁；Load/Unload；五项绑定 API；沙箱禁用项（io/os/loadstring/require 均不可用）；内存/指令/时间/栈深四类限制的触发与恢复；错误映射含行号

## 17. Integration Test

在 Scene 中挂 ScriptContext，用 Lua 实现一个「受击时触发」的脚本：C++ 发事件 → Lua 订阅 → 调用 skill.cast → C++ 结算伤害 → Lua 再收到结果。断言全链路往返正确、限制生效、脚本错误不影响 Scene Tick

## 18. Benchmark

bin/lua_bench：`lua_call_ns=` / `lua_load_ms_per_script=` / `mem_overhead_bytes=` / `instruction_check_ns=`

## 19. Failure Test

脚本死循环（while true）：指令上限触发并返回错误，**不卡死 Tick**；脚本内存超限：分配失败并返回 MemoryLimit，VM 可继续跑其他脚本；脚本调用不存在的 API：返回明确错误而非崩溃；脚本抛 error：捕获并转 ScriptError，宿主继续；脚本试图 io.open / os.execute：沙箱拒绝；脚本持有失效 EntityId：返回 NOT_FOUND 而非野指针

## 20. Acceptance Criteria

1. **每 Scene 一个 VM，不跨线程共享**（grep + 并发测试）
2. 沙箱生效：io / os / loadstring / require 全部不可用（单测断言）
3. 四类限制（内存/指令/时间/栈深）全部触发正确，且**不卡死 Tick**
4. 脚本错误被捕获，宿主进程不崩溃，Scene 继续 Tick（单测）
5. 五项绑定（Entity/Skill/Quest/Event/Query）可用且只走系统接口
6. 单次脚本调用 < 2us（benchmark 实测）
7. Debug / Release 双构建通过，ctest -R Lua 全绿

以上每一条都必须在本地真实执行并留证；**任一条不满足即判定本任务未完成**，禁止进入下一个 TASK。

## 21. Forbidden

- 禁止全局单例 LuaVM（必须每 Scene 一个）
- 禁止跨线程共享 VM
- 禁止脚本直接访问 C++ 对象裸指针（用 EntityId 句柄）
- 禁止脚本做文件 / 网络 / 数据库 IO
- 禁止脚本直接改 HP / 伤害数值（必须走系统接口）
- 禁止无指令/时间上限的脚本调用（会卡死 Tick）
- 禁止用黑名单做沙箱（必须白名单）

> 统一边界红线（全任务适用，详见 §27.3）：禁止扩散到他人 `module` 子树；下游禁止 `#include` 本任务 `src/`；禁止访问依赖模块内部数据；禁止在 `STATUS: DONE` 后静默改接口签名；禁止循环依赖。

## 22. Performance Expectation

单次脚本调用 < 2us；单 VM 常驻内存开销 < 4MB；指令计数开销 < 10%；脚本加载（100 个脚本）< 100ms。

## 23. Deliverables

- scripting/lua/include/mmo/script/script_context.h
- scripting/lua/include/mmo/script/lua_vm.h
- scripting/lua/src/*.cpp
- scripting/lua/tests/*
- scripting/lua/benchmark/*
- scripting/lua/docs/INTERFACE.md
- scripting/lua/docs/SANDBOX.md

## 24. Verification Script（本地验收）

**验收脚本**：`scripts/verify/task-031.sh`（由生成器产出，禁止手工编辑）

```bash
# 默认 Release；可指定 Debug：BUILD_TYPE=Debug bash scripts/verify/task-031.sh
bash scripts/verify/task-031.sh
```

脚本执行的检查项：

1. 前置任务门禁：`require_tasks_done 001 004 005 007 011`
2. 交付物存在性检查（4 项）
3. CMake configure + 编译（Debug + Release 双构建）
4. ctest 过滤执行：`-R Lua`
5. Benchmark 执行：`bin/lua_bench --calls 1000000`
6. 性能阈值断言：`bench/lua.txt` 中 `lua_call_ns` ≤ `2000`

脚本遵循 `set -euo pipefail`：任一步失败即非零退出，**不存在「警告通过」**。
脚本只报告真实执行结果，禁止兜底伪造 PASS；指标缺失直接判失败，禁止用估算值代替。

## 25. Git Commit

**必须先通过验收脚本（退出码 0），才允许提交。**

```bash
# 1) 把本任务标记为 DONE（脚本会校验 STATUS 流转合法）
bash scripts/task-done.sh TASK-031

# 2) 提交：Conventional Commits，scope 用模块名
git add -A
git commit -F - <<'EOF'
feat(script): Lua Runtime

- 实现要点：（填写本任务实际落地的内容，禁止复制 Objective）
- 实测数字：（粘贴 scripts/verify/task-031.sh 的真实输出，禁止写「性能良好」）

Refs: TASK-031
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
3. 查依赖：确认 TASK-001, TASK-004, TASK-005, TASK-007, TASK-011 均已 `STATUS: DONE`，否则停止并报告。
4. 查现状：grep 现有代码，确认要改的文件与符号真实存在，禁止凭空假设。
5. 守范围：只改本任务 §14 Files 范围内的文件；发现范围外问题只记录不修改。
6. 做实现：按 §15 Implementation Steps 顺序落地，每步可独立编译。
7. 本地编译：MinGW MSYS2 g++ + vcpkg manifest mode（baseline `aae277ac`），Debug 与 Release 都要过。
8. 跑单测：§16 Unit Test 全绿，新增代码必须带测试。
9. 跑集成：§17 Integration Test 全绿。
10. 跑 Benchmark：§18 真实执行，输出机器可读的 `key=value`，禁止估算。
11. 出结果：把实测数字写入 §22 对应的报告文件，不达标如实记录。
12. 跑验收脚本：`bash scripts/verify/task-031.sh` 退出码 0 后，才执行 §25 提交。

> **门禁**：本任务未通过 §20 Acceptance Criteria 与 §24 验收脚本，禁止进入下一个 TASK。

## 27. 接口契约、模块边界与扩展性

本节是**防任务间交付相互干扰 + 保框架可扩展**的统一契约，所有任务适用。

### 27.1 本任务导出的接口（冻结后不可破坏性变更）

见 §7 Public Interface。导出头只放在本任务 `include/` 下，签名一旦 `STATUS: DONE` 即视为契约冻结，下游依赖它；破坏性变更须走 `version` + 兼容性评估。

### 27.2 本任务消费的上游接口（来自前置任务，禁止绕过）

- `TASK-001` · `engine/core`：消费其 `include/` 下公开接口（详见该任务 §7 Public Interface），禁止 `#include` 其 `src/`
- `TASK-004` · `engine/core`：消费其 `include/` 下公开接口（详见该任务 §7 Public Interface），禁止 `#include` 其 `src/`
- `TASK-005` · `protocol`：消费其 `include/` 下公开接口（详见该任务 §7 Public Interface），禁止 `#include` 其 `src/`
- `TASK-007` · `engine/core`：消费其 `include/` 下公开接口（详见该任务 §7 Public Interface），禁止 `#include` 其 `src/`
- `TASK-011` · `server/gamenode/entity`：消费其 `include/` 下公开接口（详见该任务 §7 Public Interface），禁止 `#include` 其 `src/`

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
