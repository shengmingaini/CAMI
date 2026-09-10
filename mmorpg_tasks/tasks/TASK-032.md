---
TASK-ID: TASK-032
NAME: Lua Hot Reload
PHASE: Phase 7 · Lua 脚本
MODULE: scripting/lua
OWNER: Codex / WorkBuddy Agent 实施；@技术总监 二次验收；本地 MinGW MSYS2 g++ + vcpkg 编译验证
STATUS: PENDING
DEPENDENCIES: TASK-003, TASK-013, TASK-031
---

# TASK-032 · Lua Hot Reload

> 本文件由 `tools/gen/build_tasks.py` 从 `tools/gen/data_*.py` 生成，**禁止手工编辑**。
> 需要改动请修改数据源后重新生成：`python tools/gen/build_tasks.py`

| 字段 | 值 |
|---|---|
| TASK-ID | `TASK-032` |
| NAME | Lua Hot Reload |
| PHASE | Phase 7 · Lua 脚本 |
| MODULE | `scripting/lua` |
| OWNER | Codex / WorkBuddy Agent 实施；@技术总监 二次验收；本地 MinGW MSYS2 g++ + vcpkg 编译验证 |
| STATUS | **PENDING** |
| DEPENDENCIES | `TASK-003`, `TASK-013`, `TASK-031` |

---

## 1. Objective

实现 Lua 热更新：Load → Compile → Validate → Activate → Rollback → Version。**只能在 Tick Safe Point 切换，禁止在 Tick 执行中间替换脚本。**

## 2. Dependencies

### 2.1 前置任务

- `TASK-003` · Core Time / UUID / Config
- `TASK-013` · Simulation Scheduler（20Hz 固定 Tick）
- `TASK-031` · Lua Runtime

### 2.2 门禁规则

验收脚本会先执行 `require_tasks_done 003 013 031`：
任一前置任务的 `STATUS` 不是 `DONE`，脚本立即非零退出，**禁止越级实施**。

## 3. Module

`scripting/lua`

## 4. State Owner（状态归属）

当前生效脚本版本由 ScriptContext 拥有（SimulationThread）；待激活的 ReloadTicket 由 HotReloader 拥有（Worker 线程），激活瞬间原子交接。

> 硬约束：同一实时状态只能有一个权威写入者（见 PROJECT_REQUIREMENTS §10 / §12）。
> 跨模块写入必须走 Command，禁止直接改对方内存。

## 5. Input

TASK-031 ScriptContext；TASK-013 SimulationScheduler（Tick 边界安全点）；TASK-003 版本号

## 6. Output

热更模块 + 安全点切换测试 + 回滚测试 + 版本记录

## 7. Public Interface

```cpp
namespace mmo::script {
enum class ReloadState : uint8_t { Idle, Compiling, Validating, PendingActivate, Activated, RolledBack, Failed };
struct ScriptVersion { ScriptId id; uint32_t version; std::string checksum;
                       int64_t activated_at_ms; std::string activated_by; };
class HotReloader { public:
  struct Config { bool validate_before_activate{true}; uint32_t max_rollback_versions{5};
                  DurationMs validate_budget{50}; };
  // 阶段 1~3：可在任意线程（通常是运维/控制线程）
  core::Result<ReloadTicket> Prepare(std::string_view name, std::string_view source);
  core::Result<ValidationReport> Validate(ReloadTicket);     // 语法 + 沙箱 + 冒烟执行
  // 阶段 4：**只能在 Tick Safe Point 调用**，由 SimulationThread 执行
  core::Result<void> Activate(ReloadTicket, core::TraceID);
  core::Result<void> Rollback(std::string_view name, core::TraceID);
  const ScriptVersion* CurrentVersion(std::string_view name) const noexcept;
  std::vector<ScriptVersion> History(std::string_view name) const; };
}
```

## 8. Data Model

**热更流水线（六阶段，顺序不可变）**

```
1 Load      读取新源码（任意线程）
2 Compile   lua_load 编译为字节码（任意线程）
3 Validate  语法检查 + 沙箱静态检查 + 冒烟调用（任意线程）
   ─── 以上失败 = 旧版本完全不受影响 ───
4 Activate  【Tick Safe Point】原子替换 ScriptContext 中的函数引用
5 Verify    下一 Tick 冒烟验证，异常则自动 Rollback
6 Commit    记录 ScriptVersion + ConfigVersion 到审计日志
```

**版本记录**：每次成功激活必须记录 `ScriptVersion`（脚本名/版本号/checksum/时间/操作者）与 `ConfigVersion`，写入 `docs/script-versions.md` 与审计日志。
**回滚**：保留最近 5 个版本，Rollback 同样在 Tick Safe Point 执行。

## 9. Thread Model

Compile / Validate 在 Worker 线程；**Activate 与 Rollback 只能在 SimulationThread 的 Tick Safe Point**（即 Tick 八阶段全部完成之后、下一 Tick Input 之前）。禁止在 Combat/Buff 阶段中间替换。

## 10. Hot Path

**NO** （Activate 位于 Tick 边界，极短）


## 11. External IO

**YES** （读脚本文件）

所有外部 IO 必须异步化，禁止出现在 Tick 内。

## 12. Network RPC

**NO**


## 13. Persistence

**YES** （版本审计）

持久化只能经 DataService，禁止 GameNode 直连 MySQL / Redis。

## 14. Files

- scripting/lua/include/mmo/script/hot_reload/
- scripting/lua/src/hot_reload/
- scripting/lua/tests/
- docs/
- tools/scriptctl/

## 15. Implementation Steps

1. 实现 hot_reloader.h/.cpp：六阶段流水线状态机（Idle→Compiling→Validating→PendingActivate→Activated→RolledBack/Failed）
2. 实现 ReloadTicket：承载编译产物与校验报告，未 Activate 前对运行中的 VM **零影响**
3. 实现 Validate：语法检查 + 禁用 API 静态扫描 + 冒烟执行（在隔离的临时 VM 中跑，不动生产 VM）
4. 实现 Tick Safe Point 接入：向 TASK-013 SimulationScheduler 注册 Tick 边界钩子，Activate 只能在此钩子内执行
5. 实现原子替换：替换函数引用（指针赋值），**不重建 VM**（重建会丢失脚本全局状态）
6. 实现自动回滚：激活后下一 Tick 冒烟验证失败 → 自动 Rollback 并告警
7. 实现版本历史：保留最近 5 个版本（源码 checksum + 字节码），支持按版本号回滚
8. 实现审计：每次激活写日志（脚本名/版本/checksum/时间/操作者）+ 更新 docs/script-versions.md
9. 实现 CLI 工具 tools/scriptctl/：reload / validate / rollback / history / status 五个子命令
10. 写测试：编译失败不影响旧版；校验失败不影响旧版；Activate 在安全点执行（并发注入验证无中间态）；回滚正确；版本历史上限；非安全点调用 Activate 被拒绝

## 16. Unit Test

六阶段状态机全路径；编译/校验失败时旧版零影响（断言旧版仍可调用）；Activate 在非安全点返回错误；原子替换后新旧函数引用正确；回滚恢复旧版本；版本历史截断到 5 条；checksum 计算

## 17. Integration Test

**运行中热更**：Scene 以 20Hz 持续跑战斗（脚本在伤害公式中生效），运维线程发起热更 → 断言切换发生在 Tick 边界（用 Tick 号记录验证切换前后脚本版本不出现在同一 Tick 内）、切换期间无 Tick 超时、切换后新逻辑生效且状态连续（脚本全局变量保留）

## 18. Benchmark

bin/hotreload_bench：`prepare_ms=` / `validate_ms=` / `activate_us=`（安全点停顿，必须 < 100us）/ `rollback_us=`

## 19. Failure Test

热更脚本有语法错误：Validate 拦截，旧版本继续服务（**不中断线上**）；热更脚本运行时崩溃：自动 Rollback 并告警，Scene 继续跑；Activate 时正在执行该脚本：等待到安全点（不打断当前调用）；磁盘脚本文件被删：Prepare 返回 NOT_FOUND；连续 10 次热更：版本历史正确截断，内存不泄漏

## 20. Acceptance Criteria

1. **Activate 只能在 Tick Safe Point 执行**（非安全点调用返回错误，单测断言）
2. 编译失败 / 校验失败时**线上旧版本完全不受影响**（集成测试断言）
3. 单次 Activate 停顿 < 100us（不产生 Tick 尖峰，benchmark 实测）
4. 自动回滚生效（注入会崩的脚本，验证自动恢复旧版）
5. 版本历史保留最近 5 个版本，可回滚
6. 每次激活有审计日志与 docs/script-versions.md 记录
7. Debug / Release 双构建通过，ctest -R HotReload 全绿

以上每一条都必须在本地真实执行并留证；**任一条不满足即判定本任务未完成**，禁止进入下一个 TASK。

## 21. Forbidden

- 禁止在 Tick 执行中间替换脚本（必须 Tick Safe Point）
- 禁止热更时重建 VM（会丢失脚本全局状态）
- 禁止未校验直接激活
- 禁止热更失败时让线上处于半新半旧状态
- 禁止无版本记录的热更
- 禁止无限保留历史版本（内存泄漏）
- 禁止热更造成 Tick 尖峰（停顿必须 < 100us）

> 统一边界红线（全任务适用，详见 §27.3）：禁止扩散到他人 `module` 子树；下游禁止 `#include` 本任务 `src/`；禁止访问依赖模块内部数据；禁止在 `STATUS: DONE` 后静默改接口签名；禁止循环依赖。

## 22. Performance Expectation

Prepare（编译）< 10ms/脚本；Validate < 20ms/脚本；**Activate 停顿 < 100us**；Rollback < 100us；热更期间 Tick P99 无明显恶化（< 10%）。

## 23. Deliverables

- scripting/lua/include/mmo/script/hot_reload/hot_reloader.h
- scripting/lua/src/hot_reload/*.cpp
- scripting/lua/tests/*
- tools/scriptctl/*
- docs/script-versions.md
- scripting/lua/docs/HOTRELOAD.md

## 24. Verification Script（本地验收）

**验收脚本**：`scripts/verify/task-032.sh`（由生成器产出，禁止手工编辑）

```bash
# 默认 Release；可指定 Debug：BUILD_TYPE=Debug bash scripts/verify/task-032.sh
bash scripts/verify/task-032.sh
```

脚本执行的检查项：

1. 前置任务门禁：`require_tasks_done 003 013 031`
2. 交付物存在性检查（3 项）
3. CMake configure + 编译（Debug + Release 双构建）
4. ctest 过滤执行：`-R HotReload`
5. Benchmark 执行：`bin/hotreload_bench --scripts 100 --reloads 10`
6. 性能阈值断言：`bench/hotreload.txt` 中 `activate_us` ≤ `100`

脚本遵循 `set -euo pipefail`：任一步失败即非零退出，**不存在「警告通过」**。
脚本只报告真实执行结果，禁止兜底伪造 PASS；指标缺失直接判失败，禁止用估算值代替。

## 25. Git Commit

**必须先通过验收脚本（退出码 0），才允许提交。**

```bash
# 1) 把本任务标记为 DONE（脚本会校验 STATUS 流转合法）
bash scripts/task-done.sh TASK-032

# 2) 提交：Conventional Commits，scope 用模块名
git add -A
git commit -F - <<'EOF'
feat(script): Lua Hot Reload

- 实现要点：（填写本任务实际落地的内容，禁止复制 Objective）
- 实测数字：（粘贴 scripts/verify/task-032.sh 的真实输出，禁止写「性能良好」）

Refs: TASK-032
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
3. 查依赖：确认 TASK-003, TASK-013, TASK-031 均已 `STATUS: DONE`，否则停止并报告。
4. 查现状：grep 现有代码，确认要改的文件与符号真实存在，禁止凭空假设。
5. 守范围：只改本任务 §14 Files 范围内的文件；发现范围外问题只记录不修改。
6. 做实现：按 §15 Implementation Steps 顺序落地，每步可独立编译。
7. 本地编译：MinGW MSYS2 g++ + vcpkg manifest mode（baseline `aae277ac`），Debug 与 Release 都要过。
8. 跑单测：§16 Unit Test 全绿，新增代码必须带测试。
9. 跑集成：§17 Integration Test 全绿。
10. 跑 Benchmark：§18 真实执行，输出机器可读的 `key=value`，禁止估算。
11. 出结果：把实测数字写入 §22 对应的报告文件，不达标如实记录。
12. 跑验收脚本：`bash scripts/verify/task-032.sh` 退出码 0 后，才执行 §25 提交。

> **门禁**：本任务未通过 §20 Acceptance Criteria 与 §24 验收脚本，禁止进入下一个 TASK。

## 27. 接口契约、模块边界与扩展性

本节是**防任务间交付相互干扰 + 保框架可扩展**的统一契约，所有任务适用。

### 27.1 本任务导出的接口（冻结后不可破坏性变更）

见 §7 Public Interface。导出头只放在本任务 `include/` 下，签名一旦 `STATUS: DONE` 即视为契约冻结，下游依赖它；破坏性变更须走 `version` + 兼容性评估。

### 27.2 本任务消费的上游接口（来自前置任务，禁止绕过）

- `TASK-003` · `engine/core`：消费其 `include/` 下公开接口（详见该任务 §7 Public Interface），禁止 `#include` 其 `src/`
- `TASK-013` · `server/gamenode/scheduler`：消费其 `include/` 下公开接口（详见该任务 §7 Public Interface），禁止 `#include` 其 `src/`
- `TASK-031` · `scripting/lua`：消费其 `include/` 下公开接口（详见该任务 §7 Public Interface），禁止 `#include` 其 `src/`

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
