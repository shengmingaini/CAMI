---
TASK-ID: TASK-010
NAME: Gateway Router
PHASE: Phase 2 · Gateway
MODULE: server/gateway
OWNER: Codex / WorkBuddy Agent 实施；@技术总监 二次验收；本地 MinGW MSYS2 g++ + vcpkg 编译验证
STATUS: PENDING
DEPENDENCIES: TASK-007, TASK-009
---

# TASK-010 · Gateway Router

> 本文件由 `tools/gen/build_tasks.py` 从 `tools/gen/data_*.py` 生成，**禁止手工编辑**。
> 需要改动请修改数据源后重新生成：`python tools/gen/build_tasks.py`

| 字段 | 值 |
|---|---|
| TASK-ID | `TASK-010` |
| NAME | Gateway Router |
| PHASE | Phase 2 · Gateway |
| MODULE | `server/gateway` |
| OWNER | Codex / WorkBuddy Agent 实施；@技术总监 二次验收；本地 MinGW MSYS2 g++ + vcpkg 编译验证 |
| STATUS | **PENDING** |
| DEPENDENCIES | `TASK-007`, `TASK-009` |

---

## 1. Objective

实现 Gateway 路由层：PlayerRouter（PlayerID→GameNode）、SceneRouter（SceneID→GameNode）、NodeRegistry（节点注册与健康）、RouteCache。验收目标是跑通 Login → Gateway → GameNode → Scene 全链路。

## 2. Dependencies

### 2.1 前置任务

- `TASK-007` · Command / Query / Event Bus
- `TASK-009` · Session 管理

### 2.2 门禁规则

验收脚本会先执行 `require_tasks_done 007 009`：
任一前置任务的 `STATUS` 不是 `DONE`，脚本立即非零退出，**禁止越级实施**。

## 3. Module

`server/gateway`

## 4. State Owner（状态归属）

路由表（PlayerID → GameNodeID）的 Owner 是 Gateway Router；Redis 中的权威路由由 DataService / ControlService 维护，Gateway 只持有带 TTL 的本地缓存并可自建。GameNode 不拥有任何路由状态，只接受被路由到的请求。

> 硬约束：同一实时状态只能有一个权威写入者（见 PROJECT_REQUIREMENTS §10 / §12）。
> 跨模块写入必须走 Command，禁止直接改对方内存。

## 5. Input

TASK-007 Command/Event Bus；TASK-009 SessionManager；TASK-006 RPC（与 GameNode 通信）

## 6. Output

server/gateway 路由子模块 + 全链路集成测试（Login→Gateway→GameNode→Scene）

## 7. Public Interface

```cpp
namespace mmo::gateway {
struct NodeInfo { NodeId id; std::string addr; uint16_t port; NodeRole role;
                  uint32_t load{0}; core::SteadyTime last_heartbeat; NodeHealth health; };
class NodeRegistry { public:
  core::Result<void> Register(NodeInfo);
  core::Result<void> Heartbeat(NodeId, uint32_t load);
  core::Result<void> Unregister(NodeId);
  core::Result<std::vector<NodeInfo>> ListHealthy(NodeRole) const;
  core::Result<NodeInfo> Pick(NodeRole, std::string_view affinity_key = {});  // 一致性哈希
  core::Result<void> Tick(core::SteadyTime now);   // 剔除失联节点
  size_t HealthyCount(NodeRole) const noexcept; };
class PlayerRouter { public:                       // PlayerID -> GameNode
  core::Result<NodeId> Route(PlayerId);            // cache miss 时查注册中心并回填
  void Invalidate(PlayerId); void InvalidateNode(NodeId);
  size_t CacheHitRate() const noexcept;            // 指标：目标 > 99% };
class SceneRouter { public:                        // SceneID -> GameNode（Owner 唯一）
  core::Result<NodeId> OwnerOf(SceneId);
  core::Result<void> Bind(SceneId, NodeId);        // 绑定失败=已有 Owner -> VERSION_CONFLICT
  core::Result<void> Unbind(SceneId, NodeId); };
class RouteCache { public:                         // 有界 LRU，禁止无界
  explicit RouteCache(size_t capacity);
  std::optional<NodeId> Get(uint64_t key) const;
  void Put(uint64_t key, NodeId); void Erase(uint64_t); void EraseByValue(NodeId);
  size_t Size() const noexcept; double HitRate() const noexcept; };
}
```

## 8. Data Model

| 结构 | 说明 |
|---|---|
| NodeInfo | 节点身份 + 地址 + 负载 + 最后心跳 + 健康状态 |
| NodeHealth | Healthy / Suspect（1 次心跳丢失）/ Dead（3 次丢失） |
| RouteCache | 有界 LRU（默认 100k 条目），节点失效时按 value 批量失效 |
| 路由键 | PlayerID 与 SceneID 均用一致性哈希确定归属，**Scene 的 Owner 必须唯一** |

## 9. Thread Model

NodeRegistry 与 Router 由 Gateway 主（Network）线程拥有；RouteCache 分片（16 片）降低争用，禁止全局锁。健康检查由独立定时器驱动（TASK-004 Scheduler），不阻塞转发路径。

## 10. Hot Path

**YES** （每个上行包都要查路由）

本任务位于 Tick 热路径内，禁止：MySQL / Redis 同步访问 / 同步 gRPC / Kafka 同步访问 / 文件 IO / 网络阻塞 IO / 大规模内存分配（见 PROJECT_REQUIREMENTS §14）。

## 11. External IO

**NO**


## 12. Network RPC

**YES** （Gateway→GameNode 转发）

跨进程统一 gRPC + Protobuf；热路径禁止依赖远程 RPC（见 PROJECT_REQUIREMENTS §5.3）。

## 13. Persistence

**NO**


## 14. Files

- server/gateway/include/mmo/gateway/route/
- server/gateway/src/route/
- server/gateway/tests/
- server/gateway/benchmark/
- server/gateway/docs/

## 15. Implementation Steps

1. 实现 route/node_registry.h/.cpp：节点注册、心跳、健康状态机（Healthy→Suspect→Dead）、一致性哈希 Pick
2. 实现 route/route_cache.h：分片有界 LRU（16 分片，每片独立锁或无锁），支持按 value 批量失效
3. 实现 route/player_router.h/.cpp：PlayerID → NodeId，cache miss 走注册中心，命中率指标
4. 实现 route/scene_router.h/.cpp：SceneID → NodeId，**Owner 唯一性保证**（重复 Bind 返回 VERSION_CONFLICT）
5. 实现转发路径：上行包按 PlayerID 路由到目标 GameNode；未知目标时按 Scene 路由；都未知则返回 NOT_FOUND 并触发分配流程
6. 实现节点失效处理：NodeRegistry 判定 Dead → 批量失效 RouteCache 中该节点条目 → 发布 NodeDead 事件（供 TASK-037 故障迁移消费）
7. 实现负载感知：Pick 时优先低负载节点（load 来自 GameNode 心跳上报），但**不因此迁移已绑定的 Scene**
8. 写集成测试：起 1 个 Gateway + 2 个 GameNode 替身，跑通 Login → Gateway 鉴权 → 选节点 → 转发 EnterScene → GameNode 回包 → 客户端收到
9. 写测试：路由命中率；节点 Dead 后缓存批量失效；Scene 重复 Bind 失败；Cache 容量上限 LRU 淘汰；节点全部不可用时返回 BUSY 而非崩溃
10. 写 benchmark：100 万次路由查询耗时与命中率

## 16. Unit Test

NodeRegistry 注册/心跳/剔除/健康状态机；RouteCache LRU 淘汰与按值失效；PlayerRouter / SceneRouter 命中与未命中路径；Scene Owner 唯一性；一致性哈希分布均匀性（10 节点偏差 < 15%）

## 17. Integration Test

**全链路验收**：客户端 → Gateway(Login+鉴权) → PlayerRouter 选 GameNode → 转发 EnterScene 命令 → GameNode 创建/加载 Scene → 回 EnterSceneResult → Gateway 绑定 Session.SceneID → 客户端进入场景。断言 Session 七字段被正确填充（GameNodeID / SceneID 非空）

## 18. Benchmark

bin/route_bench：`route_lookup_ns=` / `cache_hit_rate=` / `cache_evict_ns=` / `registry_tick_us_1k_nodes=`

## 19. Failure Test

目标 GameNode 不可达：返回 BUSY 并触发节点健康复核，不静默丢包；GameNode 心跳停止：3 次后判定 Dead，缓存批量失效，后续请求不再发往该节点；Scene Owner 冲突（两个 GameNode 同时 Bind）：第二个返回 VERSION_CONFLICT；RouteCache 打满：LRU 淘汰且命中率不塌方（> 90%）；所有 GameNode 全挂：返回 BUSY 且有明确日志与指标

## 20. Acceptance Criteria

1. **全链路跑通**：Login → Gateway → GameNode → Scene，集成测试断言 Session 的 GameNodeID 与 SceneID 被正确写入
2. PlayerRouter 与 SceneRouter 均实现，Scene Owner 唯一性有测试保障
3. RouteCache 有界（LRU），命中率 > 99%（benchmark 实测）
4. 节点 Dead 后缓存批量失效，请求不再打向死节点（集成测试）
5. 全部节点不可用时返回 BUSY 而非崩溃
6. 路由查询 < 100ns（benchmark 实测）
7. Debug / Release 双构建通过，ctest -R Gateway_Route 全绿

以上每一条都必须在本地真实执行并留证；**任一条不满足即判定本任务未完成**，禁止进入下一个 TASK。

## 21. Forbidden

- 禁止无界 RouteCache（必须有容量上限与 LRU）
- 禁止用全局锁保护路由表
- 禁止允许同一 Scene 有两个 Owner
- 禁止在转发路径做阻塞 IO 或同步 RPC
- 禁止因负载均衡而迁移已绑定的 Scene（第一版不做 Scene 迁移）
- 禁止静默丢弃无法路由的包（必须返回错误 + 指标 + 日志）

> 统一边界红线（全任务适用，详见 §27.3）：禁止扩散到他人 `module` 子树；下游禁止 `#include` 本任务 `src/`；禁止访问依赖模块内部数据；禁止在 `STATUS: DONE` 后静默改接口签名；禁止循环依赖。

## 22. Performance Expectation

路由查询 < 100ns；缓存命中率 > 99%；1000 节点注册中心 Tick 扫描 < 100us；转发路径单次额外开销 < 1us。

## 23. Deliverables

- server/gateway/include/mmo/gateway/route/node_registry.h
- server/gateway/include/mmo/gateway/route/player_router.h
- server/gateway/include/mmo/gateway/route/scene_router.h
- server/gateway/include/mmo/gateway/route/route_cache.h
- server/gateway/src/route/*.cpp
- server/gateway/tests/*
- server/gateway/benchmark/*
- server/gateway/docs/INTERFACE.md
- server/gateway/docs/PERFORMANCE.md

## 24. Verification Script（本地验收）

**验收脚本**：`scripts/verify/task-010.sh`（由生成器产出，禁止手工编辑）

```bash
# 默认 Release；可指定 Debug：BUILD_TYPE=Debug bash scripts/verify/task-010.sh
bash scripts/verify/task-010.sh
```

脚本执行的检查项：

1. 前置任务门禁：`require_tasks_done 007 009`
2. 交付物存在性检查（1 项）
3. CMake configure + 编译（Debug + Release 双构建）
4. ctest 过滤执行：`-R Gateway_Route`
5. Benchmark 执行：`bin/route_bench --lookups 1000000`
6. 性能阈值断言：`bench/gateway_route.txt` 中 `route_lookup_ns` ≤ `100`
7. 性能阈值断言：`bench/gateway_route.txt` 中 `cache_hit_rate` ≥ `0.99`

脚本遵循 `set -euo pipefail`：任一步失败即非零退出，**不存在「警告通过」**。
脚本只报告真实执行结果，禁止兜底伪造 PASS；指标缺失直接判失败，禁止用估算值代替。

## 25. Git Commit

**必须先通过验收脚本（退出码 0），才允许提交。**

```bash
# 1) 把本任务标记为 DONE（脚本会校验 STATUS 流转合法）
bash scripts/task-done.sh TASK-010

# 2) 提交：Conventional Commits，scope 用模块名
git add -A
git commit -F - <<'EOF'
feat(server): Gateway Router

- 实现要点：（填写本任务实际落地的内容，禁止复制 Objective）
- 实测数字：（粘贴 scripts/verify/task-010.sh 的真实输出，禁止写「性能良好」）

Refs: TASK-010
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
3. 查依赖：确认 TASK-007, TASK-009 均已 `STATUS: DONE`，否则停止并报告。
4. 查现状：grep 现有代码，确认要改的文件与符号真实存在，禁止凭空假设。
5. 守范围：只改本任务 §14 Files 范围内的文件；发现范围外问题只记录不修改。
6. 做实现：按 §15 Implementation Steps 顺序落地，每步可独立编译。
7. 本地编译：MinGW MSYS2 g++ + vcpkg manifest mode（baseline `aae277ac`），Debug 与 Release 都要过。
8. 跑单测：§16 Unit Test 全绿，新增代码必须带测试。
9. 跑集成：§17 Integration Test 全绿。
10. 跑 Benchmark：§18 真实执行，输出机器可读的 `key=value`，禁止估算。
11. 出结果：把实测数字写入 §22 对应的报告文件，不达标如实记录。
12. 跑验收脚本：`bash scripts/verify/task-010.sh` 退出码 0 后，才执行 §25 提交。

> **门禁**：本任务未通过 §20 Acceptance Criteria 与 §24 验收脚本，禁止进入下一个 TASK。

## 27. 接口契约、模块边界与扩展性

本节是**防任务间交付相互干扰 + 保框架可扩展**的统一契约，所有任务适用。

### 27.1 本任务导出的接口（冻结后不可破坏性变更）

见 §7 Public Interface。导出头只放在本任务 `include/` 下，签名一旦 `STATUS: DONE` 即视为契约冻结，下游依赖它；破坏性变更须走 `version` + 兼容性评估。

### 27.2 本任务消费的上游接口（来自前置任务，禁止绕过）

- `TASK-007` · `engine/core`：消费其 `include/` 下公开接口（详见该任务 §7 Public Interface），禁止 `#include` 其 `src/`
- `TASK-009` · `server/gateway`：消费其 `include/` 下公开接口（详见该任务 §7 Public Interface），禁止 `#include` 其 `src/`

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
