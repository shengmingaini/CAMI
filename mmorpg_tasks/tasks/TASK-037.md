---
TASK-ID: TASK-037
NAME: Reconnect / Failover / Scene Recovery
PHASE: Phase 9 · 容灾
MODULE: server/gateway + server/gamenode
OWNER: Codex / WorkBuddy Agent 实施；@技术总监 二次验收；本地 MinGW MSYS2 g++ + vcpkg 编译验证
STATUS: PENDING
DEPENDENCIES: TASK-009, TASK-010, TASK-012, TASK-026, TASK-027
---

# TASK-037 · Reconnect / Failover / Scene Recovery

> 本文件由 `tools/gen/build_tasks.py` 从 `tools/gen/data_*.py` 生成，**禁止手工编辑**。
> 需要改动请修改数据源后重新生成：`python tools/gen/build_tasks.py`

| 字段 | 值 |
|---|---|
| TASK-ID | `TASK-037` |
| NAME | Reconnect / Failover / Scene Recovery |
| PHASE | Phase 9 · 容灾 |
| MODULE | `server/gateway + server/gamenode` |
| OWNER | Codex / WorkBuddy Agent 实施；@技术总监 二次验收；本地 MinGW MSYS2 g++ + vcpkg 编译验证 |
| STATUS | **PENDING** |
| DEPENDENCIES | `TASK-009`, `TASK-010`, `TASK-012`, `TASK-026`, `TASK-027` |

---

## 1. Objective

实现节点注册与健康、玩家重连、GameNode 故障接管、第一版 Scene 恢复。**第一版只要求恢复最近一次可靠持久化状态，Live Scene Migration 留到第二阶段（已登记 RFC：docs/rfc/scene-live-migration.md）。** 注意：根规范 §1 列「Scene 可以独立迁移」为长期目标，与 §34「第一版只要求重连+恢复」存在张力，本任务按 §34 执行，目标 §1 的措辞待规范层澄清。

## 2. Dependencies

### 2.1 前置任务

- `TASK-009` · Session 管理
- `TASK-010` · Gateway Router
- `TASK-012` · Scene System
- `TASK-026` · DataService Interface
- `TASK-027` · Redis Adapter

### 2.2 门禁规则

验收脚本会先执行 `require_tasks_done 009 010 012 026 027`：
任一前置任务的 `STATUS` 不是 `DONE`，脚本立即非零退出，**禁止越级实施**。

## 3. Module

`server/gateway + server/gamenode`

## 4. State Owner（状态归属）

会话状态 Owner 是 Gateway（SessionManager）；节点健康 Owner 是 HealthMonitor；Scene 状态 Owner 在恢复后转移到新 GameNode（**唯一 Owner 转移，不允许双写**）。

> 硬约束：同一实时状态只能有一个权威写入者（见 PROJECT_REQUIREMENTS §10 / §12）。
> 跨模块写入必须走 Command，禁止直接改对方内存。

## 5. Input

TASK-009 Session（含 version 防回放）；TASK-010 NodeRegistry；TASK-012 Scene；TASK-026/027 数据服务

## 6. Output

容灾模块 + 四个子项全部实现 + 故障演练报告

## 7. Public Interface

```cpp
namespace mmo::resilience {
// 37.1 Node Registry
struct NodeHealth { NodeId id; core::SteadyTime last_heartbeat; uint32_t missed;
                    uint32_t load; NodeStatus status; };   // Healthy/Suspect/Dead
class HealthMonitor { public:
  core::Result<void> Tick(core::SteadyTime now);           // 心跳扫描
  core::Result<std::vector<NodeId>> FindReplacement(NodeRole, std::string_view affinity);
  NodeStatus StatusOf(NodeId) const noexcept; };
// 37.2 Player Reconnect
enum class ReconnectStep : uint8_t { Disconnected, Reconnecting, Authenticating,
                                     LoadingPlayer, AttachingScene, Resumed, Failed };
class ReconnectService { public:
  core::Result<ReconnectStep> Begin(SessionId, net::ConnectionId, core::TraceID);
  core::Result<ReconnectStep> Advance(SessionId, core::TraceID);   // 状态机推进
  core::Result<void> OnComplete(SessionId); };
// 37.3 GameNode Failure
class FailoverCoordinator { public:
  core::Result<void> OnNodeDead(NodeId, core::TraceID);
  core::Result<void> ReattachPlayers(NodeId from, NodeId to, core::TraceID);
  FailoverStats Stats() const noexcept; };    // detected / reattached / failed / duration_ms
// 37.4 Scene Recovery
class SceneRecovery { public:
  core::Result<scene::Scene*> Restore(SceneId, NodeId new_owner, core::TraceID);
  core::Result<void> Checkpoint(const scene::Scene&);      // 定期可靠快照
  uint32_t LastCheckpointVersion(SceneId) const noexcept; };
}
```

## 8. Data Model

**37.2 重连流程（六步，顺序固定）**

```
Disconnect → Reconnect → Authenticate → Load Player → Attach Scene → Resume
   │              │            │              │              │           │
   └─ 保留 Session（grace 期）  └─ 校验 version    └─ DataService   └─ 绑定新 GameNode
```

**37.3 故障接管**：Gateway Detect（3 次心跳丢失判 Dead）→ Find Replacement（低负载同角色节点）→ Player Reattach（按 Session 重建映射）→ 恢复最近一次可靠状态。

**37.4 Scene 恢复第一版**：从最近一次 Checkpoint（默认 30 秒一次）或玩家持久化数据恢复。**明确不做** Live Scene Migration（保留实时状态迁移）—— 那是第二阶段。
**关键约束**：Redis 不作为实时状态的权威 Owner；恢复后玩家可能回到 30 秒前的位置，这是第一版可接受的设计。

## 9. Thread Model

HealthMonitor 由 Gateway 主线程 Tick 驱动；Reconnect/Failover 状态机在 Gateway 主线程；Scene 重建在目标 GameNode 的 SimulationThread。禁止跨线程共享会话状态。

## 10. Hot Path

**NO** （故障路径，非每 Tick）


## 11. External IO

**YES** （读持久化状态）

所有外部 IO 必须异步化，禁止出现在 Tick 内。

## 12. Network RPC

**YES** （跨节点协调）

跨进程统一 gRPC + Protobuf；热路径禁止依赖远程 RPC（见 PROJECT_REQUIREMENTS §5.3）。

## 13. Persistence

**YES**

持久化只能经 DataService，禁止 GameNode 直连 MySQL / Redis。

## 14. Files

- server/gateway/src/resilience/
- server/gateway/include/mmo/gateway/resilience/
- server/gamenode/scene/src/recovery/
- server/gamenode/scene/include/mmo/game/scene/recovery/
- server/gateway/tests/
- docs/

## 15. Implementation Steps

1. **37.1** 实现 health_monitor.h/.cpp：心跳扫描、Suspect/Dead 判定（3 次丢失）、替换节点选择（一致性哈希 + 负载）
2. **37.1** 实现节点健康指标：missed 心跳数、判定耗时、Dead 节点计数
3. **37.2** 实现 reconnect_service.h/.cpp：六步状态机，每步可单独失败与重试
4. **37.2** 实现 version 校验：expected_version 不匹配则拒绝（防旧连接回放，复用 TASK-009）
5. **37.2** 实现 grace 期内重连：Session 保留，玩家数据从缓存/DataService 载入
6. **37.3** 实现 failover_coordinator.h/.cpp：OnNodeDead → 批量失效路由 → 选新节点 → 批量 Reattach
7. **37.3** 实现批量限速：单次故障最多并发迁移 N 个玩家（防雪崩），其余排队
8. **37.4** 实现 Checkpoint：Scene 每 30 秒（可配）产出可靠快照（只存玩家与关键 NPC 状态，不存全部实体）
9. **37.4** 实现 Restore：从 Checkpoint 或玩家持久化数据重建 Scene，恢复后广播位置纠正
10. 实现故障演练脚本 tools/chaos/kill_gamenode.sh（本任务先做 GameNode 单项）
11. 写四个子项各自的测试 + 端到端故障演练

## 16. Unit Test

HealthMonitor 心跳与状态判定；替换节点选择；重连六步状态机各步成功/失败；version 校验；Checkpoint 产出与解析；Restore 后字段正确；批量限速

## 17. Integration Test

**端到端故障演练（必须真跑）**

| 演练 | 操作 | 断言 |
|---|---|---|
| 玩家重连 | kill 客户端连接，5 秒内重连 | 六步走完，玩家回到原 Scene，背包/属性一致 |
| GameNode 崩溃 | kill -9 一个 GameNode 进程 | Gateway 在 15 秒内检测到，玩家被分配到新节点，全部可继续游戏 |
| 会话版本攻击 | 用旧 version 请求 Reattach | 被拒绝，不影响新连接 |
| Scene 恢复 | 崩溃后恢复 Scene | 玩家状态 = 最近一次 Checkpoint，无数据损坏（无重复物品/无负余额）

## 18. Benchmark

bin/resilience_bench：`detect_ms=`（Dead 判定耗时）/ `reattach_ms_per_player=` / `scene_restore_ms=` / `checkpoint_ms=` / `checkpoint_bytes=`

## 19. Failure Test

Gateway 自身崩溃：玩家无法重连（可接受，但必须有明确日志与告警，且 Gateway 需多实例）；目标 GameNode 也不可用：重连进入 Failed 并给客户端明确提示，禁止无限重试；Checkpoint 写入失败：保留上一个 Checkpoint 并告警；玩家数据在恢复时被并发修改：版本校验拦截并重试；批量故障（1000 玩家同时重连）：限速队列生效，不雪崩

## 20. Acceptance Criteria

1. **37.1 / 37.2 / 37.3 / 37.4 四个子项全部实现**，各有独立测试
2. 端到端演练四项全部跑通（见上表，需真实 kill 进程）
3. GameNode 崩溃后 15 秒内检测 + 玩家可继续游戏
4. version 校验拒绝旧连接回放（单测）
5. 恢复后无数据损坏：无重复物品、无负余额、无属性错乱（对账校验）
6. 批量重连限速生效，不雪崩
7. **明确记录**：第一版不做 Live Scene Migration，恢复可能回退到最近 Checkpoint（写入文档）
8. Debug / Release 双构建通过，ctest -R Resilience 全绿

以上每一条都必须在本地真实执行并留证；**任一条不满足即判定本任务未完成**，禁止进入下一个 TASK。

## 21. Forbidden

- 禁止把 Redis 当作实时状态的权威 Owner
- 禁止第一版实现 Live Scene Migration（第二阶段再做）
- 禁止重连时不校验 version（会回放旧连接）
- 禁止故障转移时出现双 Owner 同时写同一 Scene
- 禁止无限重试导致雪崩（必须限速 + 退避）
- 禁止用 kill 之外的“模拟“代替真实故障演练
- 禁止把 Checkpoint 失败静默吞掉

> 统一边界红线（全任务适用，详见 §27.3）：禁止扩散到他人 `module` 子树；下游禁止 `#include` 本任务 `src/`；禁止访问依赖模块内部数据；禁止在 `STATUS: DONE` 后静默改接口签名；禁止循环依赖。

## 22. Performance Expectation

Dead 判定 < 15 秒（3 × 5s 心跳）；单玩家重连 < 500ms；1000 玩家批量接管 < 60 秒；Scene 恢复 < 3 秒；Checkpoint 产出 < 100ms、大小 < 1MB/Scene。

## 23. Deliverables

- server/gateway/include/mmo/gateway/resilience/health_monitor.h
- server/gateway/include/mmo/gateway/resilience/reconnect_service.h
- server/gateway/include/mmo/gateway/resilience/failover_coordinator.h
- server/gamenode/scene/include/mmo/game/scene/recovery/scene_recovery.h
- server/gateway/src/resilience/*.cpp
- server/gamenode/scene/src/recovery/*.cpp
- server/gateway/tests/*
- tools/chaos/kill_gamenode.sh
- docs/failover-drill-report.md

## 24. Verification Script（本地验收）

**验收脚本**：`scripts/verify/task-037.sh`（由生成器产出，禁止手工编辑）

```bash
# 默认 Release；可指定 Debug：BUILD_TYPE=Debug bash scripts/verify/task-037.sh
bash scripts/verify/task-037.sh
```

脚本执行的检查项：

1. 前置任务门禁：`require_tasks_done 009 010 012 026 027`
2. 交付物存在性检查（6 项）
3. CMake configure + 编译（Debug + Release 双构建）
4. ctest 过滤执行：`-R Resilience`
5. Benchmark 执行：`bin/resilience_bench --players 1000`
6. 性能阈值断言：`bench/resilience.txt` 中 `detect_ms` ≤ `15000`
7. 性能阈值断言：`bench/resilience.txt` 中 `reattach_ms_per_player` ≤ `500`

脚本遵循 `set -euo pipefail`：任一步失败即非零退出，**不存在「警告通过」**。
脚本只报告真实执行结果，禁止兜底伪造 PASS；指标缺失直接判失败，禁止用估算值代替。

## 25. Git Commit

**必须先通过验收脚本（退出码 0），才允许提交。**

```bash
# 1) 把本任务标记为 DONE（脚本会校验 STATUS 流转合法）
bash scripts/task-done.sh TASK-037

# 2) 提交：Conventional Commits，scope 用模块名
git add -A
git commit -F - <<'EOF'
feat(server): Reconnect / Failover / Scene Recovery

- 实现要点：（填写本任务实际落地的内容，禁止复制 Objective）
- 实测数字：（粘贴 scripts/verify/task-037.sh 的真实输出，禁止写「性能良好」）

Refs: TASK-037
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
3. 查依赖：确认 TASK-009, TASK-010, TASK-012, TASK-026, TASK-027 均已 `STATUS: DONE`，否则停止并报告。
4. 查现状：grep 现有代码，确认要改的文件与符号真实存在，禁止凭空假设。
5. 守范围：只改本任务 §14 Files 范围内的文件；发现范围外问题只记录不修改。
6. 做实现：按 §15 Implementation Steps 顺序落地，每步可独立编译。
7. 本地编译：MinGW MSYS2 g++ + vcpkg manifest mode（baseline `aae277ac`），Debug 与 Release 都要过。
8. 跑单测：§16 Unit Test 全绿，新增代码必须带测试。
9. 跑集成：§17 Integration Test 全绿。
10. 跑 Benchmark：§18 真实执行，输出机器可读的 `key=value`，禁止估算。
11. 出结果：把实测数字写入 §22 对应的报告文件，不达标如实记录。
12. 跑验收脚本：`bash scripts/verify/task-037.sh` 退出码 0 后，才执行 §25 提交。

> **门禁**：本任务未通过 §20 Acceptance Criteria 与 §24 验收脚本，禁止进入下一个 TASK。

## 27. 接口契约、模块边界与扩展性

本节是**防任务间交付相互干扰 + 保框架可扩展**的统一契约，所有任务适用。

### 27.1 本任务导出的接口（冻结后不可破坏性变更）

见 §7 Public Interface。导出头只放在本任务 `include/` 下，签名一旦 `STATUS: DONE` 即视为契约冻结，下游依赖它；破坏性变更须走 `version` + 兼容性评估。

### 27.2 本任务消费的上游接口（来自前置任务，禁止绕过）

- `TASK-009` · `server/gateway`：消费其 `include/` 下公开接口（详见该任务 §7 Public Interface），禁止 `#include` 其 `src/`
- `TASK-010` · `server/gateway`：消费其 `include/` 下公开接口（详见该任务 §7 Public Interface），禁止 `#include` 其 `src/`
- `TASK-012` · `server/gamenode/scene`：消费其 `include/` 下公开接口（详见该任务 §7 Public Interface），禁止 `#include` 其 `src/`
- `TASK-026` · `server/dataservice`：消费其 `include/` 下公开接口（详见该任务 §7 Public Interface），禁止 `#include` 其 `src/`
- `TASK-027` · `server/dataservice`：消费其 `include/` 下公开接口（详见该任务 §7 Public Interface），禁止 `#include` 其 `src/`

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
