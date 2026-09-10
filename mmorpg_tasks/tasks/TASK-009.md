---
TASK-ID: TASK-009
NAME: Session 管理
PHASE: Phase 2 · Gateway
MODULE: server/gateway
OWNER: Codex / WorkBuddy Agent 实施；@技术总监 二次验收；本地 MinGW MSYS2 g++ + vcpkg 编译验证
STATUS: DONE
DEPENDENCIES: TASK-008
---

# TASK-009 · Session 管理

> 本文件由 `tools/gen/build_tasks.py` 从 `tools/gen/data_*.py` 生成，**禁止手工编辑**。
> 需要改动请修改数据源后重新生成：`python tools/gen/build_tasks.py`

| 字段 | 值 |
|---|---|
| TASK-ID | `TASK-009` |
| NAME | Session 管理 |
| PHASE | Phase 2 · Gateway |
| MODULE | `server/gateway` |
| OWNER | Codex / WorkBuddy Agent 实施；@技术总监 二次验收；本地 MinGW MSYS2 g++ + vcpkg 编译验证 |
| STATUS | **DONE** |
| DEPENDENCIES | `TASK-008` |

---

## 1. Objective

实现 Session 与 SessionManager：会话生命周期、鉴权、心跳、断线处理与重连准备。Session 字段固定为 SessionID / PlayerID / GatewayID / GameNodeID / SceneID / Version。

## 2. Dependencies

### 2.1 前置任务

- `TASK-008` · Network Transport（TCP 第一版）

### 2.2 门禁规则

验收脚本会先执行 `require_tasks_done 008`：
任一前置任务的 `STATUS` 不是 `DONE`，脚本立即非零退出，**禁止越级实施**。

## 3. Module

`server/gateway`

## 4. State Owner（状态归属）

Session 的唯一 Owner 是持有该 Session 的 Gateway 实例（SessionStore 独占写）。SessionVersion 单调递增，只有 Gateway 能推进版本；GameNode / DataService 只持有只读缓存副本。Session 内禁止保存玩家最终持久化数据。Suspended 会话必须有 grace 超时释放，禁止无界增长。

> 硬约束：同一实时状态只能有一个权威写入者（见 PROJECT_REQUIREMENTS §10 / §12）。
> 跨模块写入必须走 Command，禁止直接改对方内存。

## 5. Input

TASK-008 传输层事件；PROJECT_REQUIREMENTS.md 第 33 节 Session 结构

## 6. Output

server/gateway 的 session 子模块 + 心跳/超时/重连测试

## 7. Public Interface

```cpp
namespace mmo::gateway {
enum class SessionState : uint8_t { Connecting, Authenticating, Active, Suspended, Closing, Closed };
struct Session {
  SessionId    session_id;    PlayerId  player_id;
  GatewayId    gateway_id;    NodeId    game_node_id;
  SceneId      scene_id;      uint32_t  version{0};
  core::SteadyTime last_heartbeat;
  SessionState state;         net::ConnectionId conn_id;
  core::TraceID trace_id;
};
class ISessionStore { public: virtual ~ISessionStore() = default;   // 为 TASK-027 Redis 预留
  virtual core::Result<void> Put(const Session&) = 0;
  virtual core::Result<std::optional<Session>> Get(SessionId) = 0;
  virtual core::Result<std::optional<Session>> FindByPlayer(PlayerId) = 0;
  virtual core::Result<void> Remove(SessionId) = 0; };
class SessionManager final {
 public:
  struct Config { DurationMs heartbeat_interval{5000}; uint32_t max_missed{3};
                  DurationMs suspend_grace{30000}; size_t max_sessions{50000}; };
  core::Result<SessionId> OnConnected(net::ConnectionId);
  core::Result<void> OnAuthenticate(SessionId, PlayerId, AuthToken);
  core::Result<void> OnHeartbeat(SessionId);
  core::Result<void> OnDisconnected(SessionId, net::CloseReason);
  core::Result<void> Reattach(SessionId, net::ConnectionId, uint32_t expected_version); // 重连核心
  core::Result<void> Tick(core::SteadyTime now);   // 扫描超时，由宿主驱动
  size_t ActiveCount() const noexcept; size_t SuspendedCount() const noexcept; };
}
```

## 8. Data Model

**Session 状态机**

```
Connecting ──auth ok──> Authenticating ──> Active
     │                       │                │
     │                   auth fail         disconnect
     ▼                       ▼                ▼
   Closed                 Closed          Suspended ──grace 内重连──> Active
                                               │
                                          grace 超时
                                               ▼
                                             Closed
```

- `version` 每次重连 +1，用于防止旧连接回放。
- Suspended 会话在 grace 期内保留，超时即释放（禁止无限堆积）。

## 9. Thread Model

SessionManager 由 Gateway 的 NetworkThread 驱动 Tick；Session 状态由单线程拥有，跨线程查询通过不可变快照或消息队列。禁止用全局锁保护 Session 表。

## 10. Hot Path

**YES** （心跳处理位于热路径）

本任务位于 Tick 热路径内，禁止：MySQL / Redis 同步访问 / 同步 gRPC / Kafka 同步访问 / 文件 IO / 网络阻塞 IO / 大规模内存分配（见 PROJECT_REQUIREMENTS §14）。

## 11. External IO

**NO**


## 12. Network RPC

**NO**


## 13. Persistence

**NO** （持久化由 TASK-027 Redis 适配器实现）


## 14. Files

- server/gateway/include/mmo/gateway/session/
- server/gateway/src/session/
- server/gateway/tests/
- server/gateway/docs/

## 15. Implementation Steps

1. 实现 session/session.h：Session 结构与 SessionId/SessionState 定义，字段严格按规范七项 + 内部字段
2. 实现 session/session_store.h：ISessionStore 接口 + InMemorySessionStore 第一版实现（为 Redis 预留同接口）
3. 实现 session/session_manager.h/.cpp：六状态机、OnConnected/OnAuthenticate/OnHeartbeat/OnDisconnected/Reattach/Tick
4. 实现心跳检测：Tick 扫描超时会话（interval × max_missed），超时转 Suspended 并发事件
5. 实现 Reattach：校验 expected_version 匹配才允许接管，防止旧连接回放；成功后 version+1 并广播 SessionReattached 事件
6. 实现容量上限：max_sessions 达到后拒绝新连接并返回 BUSY（禁止无界增长）
7. 实现鉴权接口抽象 IAuthProvider（第一版用本地 token 校验替身，禁止写死口令）
8. 实现会话事件：SessionCreated / SessionAuthenticated / SessionSuspended / SessionResumed / SessionClosed，全部走 EventBus
9. 写测试：状态机全路径（含非法转移返回错误）；心跳超时转 Suspended；grace 内 Reattach 成功；grace 超时释放；version 不匹配的 Reattach 被拒绝；容量上限行为
10. 写集成测试：模拟 1000 会话心跳压测 + 100 会话同时断线重连
11. 写 docs/INTERFACE.md 与 docs/README.md（含状态机图）

## 16. Unit Test

六状态机合法/非法转移全覆盖；心跳计时准确；version 递增与校验；容量上限；SessionStore CRUD；事件发布正确

## 17. Integration Test

1000 个会话并发心跳，Tick 扫描耗时可测且 < 1ms；模拟 100 个会话同时断线后 5 秒内全部重连成功且 version 正确递增；断线期间发往该会话的消息被丢弃或缓存（按配置）且不下发给旧连接

## 18. Benchmark

bin/session_bench：`session_heartbeat_ns=` / `session_tick_us_10k=` / `reattach_ns=` / `per_session_bytes=`

## 19. Failure Test

心跳风暴（1 万会话同一秒发心跳）：不丢、不崩、Tick 耗时可测；鉴权失败：转 Closed 并记录，不保留悬挂会话；Reattach 时旧连接仍在线：旧连接被强制关闭且事件顺序正确；SessionStore 不可用（注入故障）：返回明确错误，不吞异常；grace 期内的会话不会被误释放（边界时间测试）

## 20. Acceptance Criteria

1. Session 七项字段（SessionID/PlayerID/GatewayID/GameNodeID/SceneID/Version + 心跳）全部实现
2. 六状态机全部路径有单测，非法转移返回 INVALID_ARGUMENT
3. version 不匹配的 Reattach 被拒绝（防回放，单测断言）
4. grace 期内可重连、超期释放，两个边界都有测试
5. 1000 会话心跳 Tick 扫描 < 1ms（benchmark 实测）
6. 会话容量上限生效，不无界增长
7. SessionManager 不使用全局锁（代码评审确认）
8. Debug / Release 双构建通过，ctest -R Gateway_Session 全绿

以上每一条都必须在本地真实执行并留证；**任一条不满足即判定本任务未完成**，禁止进入下一个 TASK。

## 21. Forbidden

- 禁止用全局锁保护 Session 表
- 禁止无限保留 Suspended 会话（必须 grace 超时释放）
- 禁止 Reattach 时不校验 version
- 禁止在 Session 中保存玩家最终持久化数据（那是 DataService 职责）
- 禁止把鉴权口令写死在代码里
- 禁止在心跳处理中做阻塞 IO

> 统一边界红线（全任务适用，详见 §27.3）：禁止扩散到他人 `module` 子树；下游禁止 `#include` 本任务 `src/`；禁止访问依赖模块内部数据；禁止在 `STATUS: DONE` 后静默改接口签名；禁止循环依赖。

## 22. Performance Expectation

10K 会话心跳 Tick 扫描 < 1ms；单会话内存占用 < 256B；Reattach 处理 < 10us；心跳处理 < 200ns/次。

## 23. Deliverables

- server/gateway/include/mmo/gateway/session/session.h
- server/gateway/include/mmo/gateway/session/session_manager.h
- server/gateway/include/mmo/gateway/session/session_store.h
- server/gateway/src/session/*.cpp
- server/gateway/tests/*
- server/gateway/docs/INTERFACE.md
- server/gateway/docs/README.md

## 24. Verification Script（本地验收）

**验收脚本**：`scripts/verify/task-009.sh`（由生成器产出，禁止手工编辑）

```bash
# 默认 Release；可指定 Debug：BUILD_TYPE=Debug bash scripts/verify/task-009.sh
bash scripts/verify/task-009.sh
```

脚本执行的检查项：

1. 前置任务门禁：`require_tasks_done 008`
2. 交付物存在性检查（5 项）
3. CMake configure + 编译（Debug + Release 双构建）
4. ctest 过滤执行：`-R Gateway_Session`
5. Benchmark 执行：`bin/session_bench --sessions 10000`
6. 性能阈值断言：`bench/gateway_session.txt` 中 `session_tick_us_10k` ≤ `1000`
7. 性能阈值断言：`bench/gateway_session.txt` 中 `per_session_bytes` ≤ `256`

脚本遵循 `set -euo pipefail`：任一步失败即非零退出，**不存在「警告通过」**。
脚本只报告真实执行结果，禁止兜底伪造 PASS；指标缺失直接判失败，禁止用估算值代替。

## 25. Git Commit

**必须先通过验收脚本（退出码 0），才允许提交。**

```bash
# 1) 把本任务标记为 DONE（脚本会校验 STATUS 流转合法）
bash scripts/task-done.sh TASK-009

# 2) 提交：Conventional Commits，scope 用模块名
git add -A
git commit -F - <<'EOF'
feat(server): Session 管理

- 实现要点：（填写本任务实际落地的内容，禁止复制 Objective）
- 实测数字：（粘贴 scripts/verify/task-009.sh 的真实输出，禁止写「性能良好」）

Refs: TASK-009
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
3. 查依赖：确认 TASK-008 均已 `STATUS: DONE`，否则停止并报告。
4. 查现状：grep 现有代码，确认要改的文件与符号真实存在，禁止凭空假设。
5. 守范围：只改本任务 §14 Files 范围内的文件；发现范围外问题只记录不修改。
6. 做实现：按 §15 Implementation Steps 顺序落地，每步可独立编译。
7. 本地编译：MinGW MSYS2 g++ + vcpkg manifest mode（baseline `aae277ac`），Debug 与 Release 都要过。
8. 跑单测：§16 Unit Test 全绿，新增代码必须带测试。
9. 跑集成：§17 Integration Test 全绿。
10. 跑 Benchmark：§18 真实执行，输出机器可读的 `key=value`，禁止估算。
11. 出结果：把实测数字写入 §22 对应的报告文件，不达标如实记录。
12. 跑验收脚本：`bash scripts/verify/task-009.sh` 退出码 0 后，才执行 §25 提交。

> **门禁**：本任务未通过 §20 Acceptance Criteria 与 §24 验收脚本，禁止进入下一个 TASK。

## 27. 接口契约、模块边界与扩展性

本节是**防任务间交付相互干扰 + 保框架可扩展**的统一契约，所有任务适用。

### 27.1 本任务导出的接口（冻结后不可破坏性变更）

见 §7 Public Interface。导出头只放在本任务 `include/` 下，签名一旦 `STATUS: DONE` 即视为契约冻结，下游依赖它；破坏性变更须走 `version` + 兼容性评估。

### 27.2 本任务消费的上游接口（来自前置任务，禁止绕过）

- `TASK-008` · `engine/net`：消费其 `include/` 下公开接口（详见该任务 §7 Public Interface），禁止 `#include` 其 `src/`

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
