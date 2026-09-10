---
TASK-ID: TASK-040
NAME: ControlService（控制面：节点管理/配置下发/健康/运维）
PHASE: Phase 9 · 运维控制面
MODULE: server/control
OWNER: Codex / WorkBuddy Agent 实施；@技术总监 二次验收；本地 MinGW MSYS2 g++ + vcpkg 编译验证
STATUS: PENDING
DEPENDENCIES: TASK-003, TASK-006, TASK-010
---

# TASK-040 · ControlService（控制面：节点管理/配置下发/健康/运维）

> 本文件由 `tools/gen/build_tasks.py` 从 `tools/gen/data_*.py` 生成，**禁止手工编辑**。
> 需要改动请修改数据源后重新生成：`python tools/gen/build_tasks.py`

| 字段 | 值 |
|---|---|
| TASK-ID | `TASK-040` |
| NAME | ControlService（控制面：节点管理/配置下发/健康/运维） |
| PHASE | Phase 9 · 运维控制面 |
| MODULE | `server/control` |
| OWNER | Codex / WorkBuddy Agent 实施；@技术总监 二次验收；本地 MinGW MSYS2 g++ + vcpkg 编译验证 |
| STATUS | **PENDING** |
| DEPENDENCIES | `TASK-003`, `TASK-006`, `TASK-010` |

---

## 1. Objective

实现集群控制面 ControlService：**节点注册表权威同步**（与 TASK-010 NodeRegistry 对接）、**配置版本（Config Version）下发**、**健康检查与运维接口**、**Gateway 多实例注册**。它不拥有任何游戏状态，只做控制与协调。灰度发布与自动扩缩容列为 Phase 2（见 docs/rfc/）。这是补齐「四进程」中唯一缺失的实现。

## 2. Dependencies

### 2.1 前置任务

- `TASK-003` · Core Time / UUID / Config
- `TASK-006` · RPC Framework（gRPC 统一封装）
- `TASK-010` · Gateway Router

### 2.2 门禁规则

验收脚本会先执行 `require_tasks_done 003 006 010`：
任一前置任务的 `STATUS` 不是 `DONE`，脚本立即非零退出，**禁止越级实施**。

## 3. Module

`server/control`

## 4. State Owner（状态归属）

ControlService 是集群控制面：节点注册表（NodeRegistry 的权威同步方）、配置版本（Config Version）下发、健康检查/运维接口、Gateway 多实例注册的权威。它不拥有任何游戏状态，只做控制与协调；灰度发布与自动扩缩容列为 Phase 2。

> 硬约束：同一实时状态只能有一个权威写入者（见 PROJECT_REQUIREMENTS §10 / §12）。
> 跨模块写入必须走 Command，禁止直接改对方内存。

## 5. Input

TASK-003 ConfigManager（Config Snapshot + 版本）；TASK-006 gRPC（HealthService/EchoService 已定义）；TASK-010 NodeRegistry（节点注册/健康/路由缓存）

## 6. Output

ControlService 进程（或 GameNode 内控制线程）+ 节点管理接口 + 配置下发通道 + 运维接口 + 文档

## 7. Public Interface

```proto
// control_service.proto（增量扩展 TASK-006 的 service 定义）
service ControlService {
  rpc RegisterNode(RegisterNodeReq)   returns (RegisterNodeResp);   // GameNode/Gateway 上线注册
  rpc Heartbeat(HeartbeatReq)         returns (HeartbeatResp);      // 健康维持，含负载（player_count/tick_p99）
  rpc PushConfig(PushConfigReq)       returns (PushConfigResp);     // 携带 config_version，节点比对后热加载
  rpc QueryTopology(QueryTopologyReq) returns (QueryTopologyResp);  // 当前节点/路由拓扑（运维用）
}
message RegisterNodeReq { string node_id; string role; string addr; uint32 capacity; }
message PushConfigReq   { uint64 config_version; bytes snapshot; }
```

## 8. Data Model

**控制面数据（非游戏状态）**

- 节点表：`node_id → {role, addr, capacity, load, last_heartbeat, status}`，权威在 ControlService，Gateway/GameNode 持缓存。
- 配置版本：`config_version` 单调递增；节点收到 PushConfig 后比对本地版本，落后则热加载（复用 TASK-003 的原子指针替换）。
- 路由权威：TASK-010 说 Redis 路由权威归 DataService/ControlService，本任务把「节点上下线 → 路由缓存失效」的协调做掉。

**Gateway 多实例**：Gateway 注册到节点表后，前置 LB（四层/Envoy）按 `capacity/load` 分流；本任务不实现 LB 本身，只提供注册与健康数据。

## 9. Thread Model

ControlService 独立进程（或 GameNode 内控制线程），事件驱动；心跳处理无锁更新节点表（单写者模型）。不与游戏 Simulation 争锁。

## 10. Hot Path

**NO**


## 11. External IO

**YES** （健康/配置落盘与下发）

所有外部 IO 必须异步化，禁止出现在 Tick 内。

## 12. Network RPC

**YES** （gRPC ControlService）

跨进程统一 gRPC + Protobuf；热路径禁止依赖远程 RPC（见 PROJECT_REQUIREMENTS §5.3）。

## 13. Persistence

**YES** （节点表/配置版本持久化，可选 Redis）

持久化只能经 DataService，禁止 GameNode 直连 MySQL / Redis。

## 14. Files

- server/control/include/mmo/control/
- server/control/src/
- server/control/tests/
- server/control/docs/INTERFACE.md
- config/control/

## 15. Implementation Steps

1. 定义 control_service.proto（RegisterNode/Heartbeat/PushConfig/QueryTopology），并入 TASK-006 的 proto 目录
2. 实现节点注册表：GameNode/Gateway 启动注册、心跳维持负载、超时判定离线
3. 实现配置下发：监听 config_version，落后则拉取并热加载（复用 TASK-003 原子替换）
4. 实现拓扑查询与运维接口（节点列表/路由/容量），供排查与容量报告使用
5. 对接 TASK-010 NodeRegistry：节点上下线触发路由缓存失效协调
6. Gateway 多实例注册：Gateway 注册到节点表，输出 `capacity/load` 供前置 LB 分流
7. 写 INTERFACE.md：导出接口 + 消费的上游（TASK-003/006/010）+ 灰度/自动扩缩容的 Phase 2 边界说明
8. 集成测试：节点上下线 → 路由缓存失效 → 新节点可承接；配置版本推进 → 节点热加载

## 16. Unit Test

节点注册/心跳/超时/配置版本比对各分支正确；路由失效协调在节点离线后触发

## 17. Integration Test

起一个 GameNode + ControlService：GameNode 注册→心跳→ControlService 可见；kill GameNode→超时→ControlService 标记离线→路由失效；推进 config_version→GameNode 热加载新配置

## 18. Benchmark

无（控制面低频）；记录单 ControlService 可管理节点数基线（> 1000 节点心跳不超时）

## 19. Failure Test

ControlService 崩溃：游戏节点降级为「用本地缓存路由继续服务」，不雪崩（明确记录降级行为）；配置下发失败：节点保留旧版本并告警，不中断服务

## 20. Acceptance Criteria

1. 节点注册/心跳/超时/配置下发/拓扑查询全部实现并集成测试通过
2. 节点上下线正确触发 TASK-010 路由缓存失效协调
3. config_version 推进后节点热加载新配置（复用 TASK-003 原子替换，无锁）
4. Gateway 多实例注册并输出 capacity/load（前置 LB 分流所需数据齐备）
5. ControlService 崩溃时游戏节点降级不雪崩（集成测试断言）
6. INTERFACE.md 列出导出接口与消费的上游接口；`include/` 未泄露 `src/`
7. Debug / Release 双构建通过，ctest -R Control 全绿

以上每一条都必须在本地真实执行并留证；**任一条不满足即判定本任务未完成**，禁止进入下一个 TASK。

## 21. Forbidden

- 禁止 ControlService 拥有任何游戏状态（玩家/Scene/Entity 等）
- 禁止在 GameNode 内硬编码节点表（必须来自 ControlService 下发）
- 禁止配置变更破坏旧版本兼容（必须带 config_version 且可回退）
- 禁止把灰度发布/自动扩缩容塞进第一版（明确列为 Phase 2 RFC）

> 统一边界红线（全任务适用，详见 §27.3）：禁止扩散到他人 `module` 子树；下游禁止 `#include` 本任务 `src/`；禁止访问依赖模块内部数据；禁止在 `STATUS: DONE` 后静默改接口签名；禁止循环依赖。

## 22. Performance Expectation

单 ControlService 管理 > 1000 节点心跳不超时；配置下发延迟 < 1s（千节点）；路由失效协调在节点离线超时后 < 1s 内完成。

## 23. Deliverables

- server/control/include/mmo/control/control_service.h
- server/control/src/*.cpp
- server/control/tests/*
- protocol/proto/service/control_service.proto
- server/control/docs/INTERFACE.md

## 24. Verification Script（本地验收）

**验收脚本**：`scripts/verify/task-040.sh`（由生成器产出，禁止手工编辑）

```bash
# 默认 Release；可指定 Debug：BUILD_TYPE=Debug bash scripts/verify/task-040.sh
bash scripts/verify/task-040.sh
```

脚本执行的检查项：

1. 前置任务门禁：`require_tasks_done 003 006 010`
2. 交付物存在性检查（3 项）
3. CMake configure + 编译（Debug + Release 双构建）
4. ctest 过滤执行：`-R Control`

脚本遵循 `set -euo pipefail`：任一步失败即非零退出，**不存在「警告通过」**。
脚本只报告真实执行结果，禁止兜底伪造 PASS；指标缺失直接判失败，禁止用估算值代替。

## 25. Git Commit

**必须先通过验收脚本（退出码 0），才允许提交。**

```bash
# 1) 把本任务标记为 DONE（脚本会校验 STATUS 流转合法）
bash scripts/task-done.sh TASK-040

# 2) 提交：Conventional Commits，scope 用模块名
git add -A
git commit -F - <<'EOF'
feat(server): ControlService（控制面：节点管理/配置下发/健康/运维）

- 实现要点：（填写本任务实际落地的内容，禁止复制 Objective）
- 实测数字：（粘贴 scripts/verify/task-040.sh 的真实输出，禁止写「性能良好」）

Refs: TASK-040
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
3. 查依赖：确认 TASK-003, TASK-006, TASK-010 均已 `STATUS: DONE`，否则停止并报告。
4. 查现状：grep 现有代码，确认要改的文件与符号真实存在，禁止凭空假设。
5. 守范围：只改本任务 §14 Files 范围内的文件；发现范围外问题只记录不修改。
6. 做实现：按 §15 Implementation Steps 顺序落地，每步可独立编译。
7. 本地编译：MinGW MSYS2 g++ + vcpkg manifest mode（baseline `aae277ac`），Debug 与 Release 都要过。
8. 跑单测：§16 Unit Test 全绿，新增代码必须带测试。
9. 跑集成：§17 Integration Test 全绿。
10. 跑 Benchmark：§18 真实执行，输出机器可读的 `key=value`，禁止估算。
11. 出结果：把实测数字写入 §22 对应的报告文件，不达标如实记录。
12. 跑验收脚本：`bash scripts/verify/task-040.sh` 退出码 0 后，才执行 §25 提交。

> **门禁**：本任务未通过 §20 Acceptance Criteria 与 §24 验收脚本，禁止进入下一个 TASK。

## 27. 接口契约、模块边界与扩展性

本节是**防任务间交付相互干扰 + 保框架可扩展**的统一契约，所有任务适用。

### 27.1 本任务导出的接口（冻结后不可破坏性变更）

见 §7 Public Interface。导出头只放在本任务 `include/` 下，签名一旦 `STATUS: DONE` 即视为契约冻结，下游依赖它；破坏性变更须走 `version` + 兼容性评估。

### 27.2 本任务消费的上游接口（来自前置任务，禁止绕过）

- `TASK-003` · `engine/core`：消费其 `include/` 下公开接口（详见该任务 §7 Public Interface），禁止 `#include` 其 `src/`
- `TASK-006` · `engine/rpc`：消费其 `include/` 下公开接口（详见该任务 §7 Public Interface），禁止 `#include` 其 `src/`
- `TASK-010` · `server/gateway`：消费其 `include/` 下公开接口（详见该任务 §7 Public Interface），禁止 `#include` 其 `src/`

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
