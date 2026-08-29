---
TASK-ID: TASK-039
NAME: Social System（组队/好友/公会/聊天/邮件）
PHASE: Phase 7 · 社交系统
MODULE: server/gamenode/social
OWNER: Codex / WorkBuddy Agent 实施；@技术总监 二次验收；本地 MinGW MSYS2 g++ + vcpkg 编译验证
STATUS: PENDING
DEPENDENCIES: TASK-007, TASK-011, TASK-016, TASK-028
---

# TASK-039 · Social System（组队/好友/公会/聊天/邮件）

> 本文件由 `tools/gen/build_tasks.py` 从 `tools/gen/data_*.py` 生成，**禁止手工编辑**。
> 需要改动请修改数据源后重新生成：`python tools/gen/build_tasks.py`

| 字段 | 值 |
|---|---|
| TASK-ID | `TASK-039` |
| NAME | Social System（组队/好友/公会/聊天/邮件） |
| PHASE | Phase 7 · 社交系统 |
| MODULE | `server/gamenode/social` |
| OWNER | Codex / WorkBuddy Agent 实施；@技术总监 二次验收；本地 MinGW MSYS2 g++ + vcpkg 编译验证 |
| STATUS | **PENDING** |
| DEPENDENCIES | `TASK-007`, `TASK-011`, `TASK-016`, `TASK-028` |

---

## 1. Objective

实现社交运行时：Party（组队）/ Friend（好友）/ Guild（公会）/ Chat（聊天）/ Mail（邮件）。第一版**单 GameNode 内全量内存态**，跨节点社交由 Gateway 路由 + DataService 持久化副本兜底。所有社交动作走 Command / Event，禁止直接改 Role 或 Scene 数据。**这是对「类大型 MMORPG」功能完整度的补齐。**

## 2. Dependencies

### 2.1 前置任务

- `TASK-007` · Command / Query / Event Bus
- `TASK-011` · Entity System
- `TASK-016` · Player / Character
- `TASK-028` · MySQL Adapter

### 2.2 门禁规则

验收脚本会先执行 `require_tasks_done 007 011 016 028`：
任一前置任务的 `STATUS` 不是 `DONE`，脚本立即非零退出，**禁止越级实施**。

## 3. Module

`server/gamenode/social`

## 4. State Owner（状态归属）

社交状态（组队/好友/公会/邮件）的运行时权威归 Social System（GameNode 内，单节点内全量内存态）；跨节点社交由 Gateway 路由 + 持久化副本兜底。公会/邮件的持久化权威归 DataService。社交事件（邀请/入会/私聊）走 EventBus，禁止直接改对方 Role 或 Scene 数据。

> 硬约束：同一实时状态只能有一个权威写入者（见 PROJECT_REQUIREMENTS §10 / §12）。
> 跨模块写入必须走 Command，禁止直接改对方内存。

## 5. Input

TASK-007 Command/Event Bus；TASK-011 Entity（玩家实体引用）；TASK-016 Role（玩家档案）；TASK-028 MySQL（Guild/Mail 表已存在）

## 6. Output

Social 模块（Party/Friend/Guild/Chat/Mail）+ 社交事件定义 + 单 GameNode 集成测试 + 接口文档

## 7. Public Interface

```cpp
namespace mmo::game::social {
enum class SocialOp : uint8_t { Invite, Accept, Decline, Leave, Kick, Promote,
                                AddFriend, RemoveFriend, SendMail, ClaimMail, GuildCreate, GuildJoin };
struct Party { party_id; std::vector<player_id> members; player_id leader; scene_id home; };
struct Guild { guild_id; std::string name; player_id leader; uint32_t member_count; };
class SocialSystem { public:
  // 全部经 Command 进入，返回 Result；禁止内部直改 Role
  core::Result<party_id> CreateParty(player_id leader);
  core::Result<void>   Invite(player_id from, player_id to);
  core::Result<void>   AcceptInvite(player_id who, party_id pid);
  core::Result<void>   Leave(player_id who);
  core::Result<guild_id> CreateGuild(player_id leader, std::string_view name);
  core::Result<void>   JoinGuild(player_id who, guild_id gid);
  core::Result<void>   SendMail(player_id from, player_id to, std::string_view payload);
  // 事件统一发出：PartyEvent / GuildEvent / ChatEvent / MailEvent
};
}
```

## 8. Data Model

**内存模型（第一版，单 GameNode 内全量）**

- Party：GameNode 内存 `unordered_map<party_id, Party>`，成员上限可配（默认 5）。
- Friend：每玩家 `set<player_id>`，挂 Role 之下，持久化经 DataService。
- Guild：内存 `unordered_map<guild_id, Guild>` + 成员表；权威持久化在 MySQL（TASK-028 表已建）。
- Chat：频道（私聊/队伍/公会/世界）由 ChatService 在 GameNode 内路由；**世界频道走 Gateway 广播而非全服 O(N) 扫描**（复用 AOI 邻域或订阅表）。
- Mail：写入经 Economy/Ledger 同款幂等通道，读取走 Query。

**扩展点**：新增社交关系类型必须走 `SocialOp` 枚举 + 注册表，禁止在 handler 里硬编码穷举。

## 9. Thread Model

SocialSystem 运行在 Scene/GameNode 主 Simulation 线程（同其它 Gameplay 模块），不新建线程；Guild 持久化写走 DataService 异步，热路径不触 MySQL。

## 10. Hot Path

**NO** （社交事件低频）


## 11. External IO

**YES** （Guild/Mail 持久化经 DataService）

所有外部 IO 必须异步化，禁止出现在 Tick 内。

## 12. Network RPC

**NO** （第一版单 GameNode 内）


## 13. Persistence

**YES** （Guild/Mail 落 MySQL）

持久化只能经 DataService，禁止 GameNode 直连 MySQL / Redis。

## 14. Files

- server/gamenode/social/include/mmo/game/social/
- server/gamenode/social/src/
- server/gamenode/social/tests/
- server/gamenode/social/docs/INTERFACE.md
- config/gameplay/social.json

## 15. Implementation Steps

1. 实现 SocialSystem 骨架与 `SocialOp` 枚举 + 注册表（禁止 switch 硬编码穷举）
2. 实现 Party：创建/邀请/接受/离开/踢人/队长转移；成员上限可配；状态写回 Role 经 Command
3. 实现 Friend：加/删好友，双向一致，持久化经 DataService
4. 实现 Guild：创建/加入/退出/成员管理/权限（leader/member）；内存态 + MySQL 持久化双写（异步）
5. 实现 Chat：私聊/队伍/公会/世界四频道；世界频道经 Gateway 订阅表而非全服扫描
6. 实现 Mail：发信/收信/领取附件（附件走 Economy 幂等通道，复用 TASK-030）；过期清理
7. 定义社交事件（PartyEvent/GuildEvent/ChatEvent/MailEvent）并入 EventBus（TASK-007）
8. 写 INTERFACE.md：导出头清单 + 消费的上游接口（TASK-007/011/016/028）
9. 集成测试：组队进本（跨 Scene 走 Command，复用 TASK-020）、公会创建+持久化往返、私聊端到端

## 16. Unit Test

各社交操作的 Result 正确（重复邀请/已存在/权限不足等错误码）；Guild 双写一致性（内存==MySQL 快照）；事件触发计数正确

## 17. Integration Test

单 GameNode 内：A 邀 B 组队→B 接受→两人同 Party；A 建公会→B 加入→MySQL 可见；A 私聊 B→B 收到 ChatEvent；邮件领取幂等（重复领取只发一次附件）

## 18. Benchmark

无独立 benchmark（社交事件低频）；可统计单 GameNode 社交操作吞吐（> 1k ops/s）作为容量基线

## 19. Failure Test

Guild 持久化失败：内存态保留、标记待重试、不丢数据；世界频道在高在线下不退化成全服 O(N) 广播；邮件重复领取：第二次返回 AlreadyClaimed，不重复发附件

## 20. Acceptance Criteria

1. Party/Friend/Guild/Chat/Mail 五项全部可运行并集成测试通过
2. 所有社交写操作经 Command/Event，grep 确认无直接改 Role/Scene 私有成员
3. Guild 内存态与 MySQL 持久化一致（集成测试断言往返）
4. 世界频道走 Gateway 订阅表，禁止出现全服玩家遍历广播（代码评审 + 静态审查）
5. 邮件领取幂等：重复领取只发一次附件（复用 TASK-030 幂等键）
6. INTERFACE.md 列出导出头与消费的上游接口；`include/` 公开头未泄露 `src/`
7. Debug / Release 双构建通过，ctest -R Social 全绿

以上每一条都必须在本地真实执行并留证；**任一条不满足即判定本任务未完成**，禁止进入下一个 TASK。

## 21. Forbidden

- 禁止在 handler 中 switch 硬编码穷举社交类型（必须走注册表/枚举扩展）
- 禁止直接改 Role 或 Scene 的私有数据（必须经 Command/接口）
- 禁止世界频道用全服 O(N) 玩家遍历广播
- 禁止为社交新建独立线程（沿用 Simulation 线程）
- 禁止把 Guild/Mail 持久化逻辑写死在业务逻辑里（必须走 DataService）

> 统一边界红线（全任务适用，详见 §27.3）：禁止扩散到他人 `module` 子树；下游禁止 `#include` 本任务 `src/`；禁止访问依赖模块内部数据；禁止在 `STATUS: DONE` 后静默改接口签名；禁止循环依赖。

## 22. Performance Expectation

单 GameNode 社交操作吞吐 > 1k ops/s；Guild 双写额外延迟 < 1ms（异步）；世界频道单消息投递成本与接收者数量成正比、与全服人数无关。

## 23. Deliverables

- server/gamenode/social/include/mmo/game/social/social_system.h
- server/gamenode/social/src/*.cpp
- server/gamenode/social/tests/*
- config/gameplay/social.json
- server/gamenode/social/docs/INTERFACE.md

## 24. Verification Script（本地验收）

**验收脚本**：`scripts/verify/task-039.sh`（由生成器产出，禁止手工编辑）

```bash
# 默认 Release；可指定 Debug：BUILD_TYPE=Debug bash scripts/verify/task-039.sh
bash scripts/verify/task-039.sh
```

脚本执行的检查项：

1. 前置任务门禁：`require_tasks_done 007 011 016 028`
2. 交付物存在性检查（1 项）
3. CMake configure + 编译（Debug + Release 双构建）
4. ctest 过滤执行：`-R Social`

脚本遵循 `set -euo pipefail`：任一步失败即非零退出，**不存在「警告通过」**。
脚本只报告真实执行结果，禁止兜底伪造 PASS；指标缺失直接判失败，禁止用估算值代替。

## 25. Git Commit

**必须先通过验收脚本（退出码 0），才允许提交。**

```bash
# 1) 把本任务标记为 DONE（脚本会校验 STATUS 流转合法）
bash scripts/task-done.sh TASK-039

# 2) 提交：Conventional Commits，scope 用模块名
git add -A
git commit -F - <<'EOF'
feat(server): Social System（组队/好友/公会/聊天/邮件）

- 实现要点：（填写本任务实际落地的内容，禁止复制 Objective）
- 实测数字：（粘贴 scripts/verify/task-039.sh 的真实输出，禁止写「性能良好」）

Refs: TASK-039
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
3. 查依赖：确认 TASK-007, TASK-011, TASK-016, TASK-028 均已 `STATUS: DONE`，否则停止并报告。
4. 查现状：grep 现有代码，确认要改的文件与符号真实存在，禁止凭空假设。
5. 守范围：只改本任务 §14 Files 范围内的文件；发现范围外问题只记录不修改。
6. 做实现：按 §15 Implementation Steps 顺序落地，每步可独立编译。
7. 本地编译：MinGW MSYS2 g++ + vcpkg manifest mode（baseline `aae277ac`），Debug 与 Release 都要过。
8. 跑单测：§16 Unit Test 全绿，新增代码必须带测试。
9. 跑集成：§17 Integration Test 全绿。
10. 跑 Benchmark：§18 真实执行，输出机器可读的 `key=value`，禁止估算。
11. 出结果：把实测数字写入 §22 对应的报告文件，不达标如实记录。
12. 跑验收脚本：`bash scripts/verify/task-039.sh` 退出码 0 后，才执行 §25 提交。

> **门禁**：本任务未通过 §20 Acceptance Criteria 与 §24 验收脚本，禁止进入下一个 TASK。

## 27. 接口契约、模块边界与扩展性

本节是**防任务间交付相互干扰 + 保框架可扩展**的统一契约，所有任务适用。

### 27.1 本任务导出的接口（冻结后不可破坏性变更）

见 §7 Public Interface。导出头只放在本任务 `include/` 下，签名一旦 `STATUS: DONE` 即视为契约冻结，下游依赖它；破坏性变更须走 `version` + 兼容性评估。

### 27.2 本任务消费的上游接口（来自前置任务，禁止绕过）

- `TASK-007` · `engine/core`：消费其 `include/` 下公开接口（详见该任务 §7 Public Interface），禁止 `#include` 其 `src/`
- `TASK-011` · `server/gamenode/entity`：消费其 `include/` 下公开接口（详见该任务 §7 Public Interface），禁止 `#include` 其 `src/`
- `TASK-016` · `server/gamenode/role`：消费其 `include/` 下公开接口（详见该任务 §7 Public Interface），禁止 `#include` 其 `src/`
- `TASK-028` · `server/dataservice`：消费其 `include/` 下公开接口（详见该任务 §7 Public Interface），禁止 `#include` 其 `src/`

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
