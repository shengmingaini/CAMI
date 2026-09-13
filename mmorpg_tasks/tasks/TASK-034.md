---
TASK-ID: TASK-034
NAME: Client Core
PHASE: Phase 8 · 客户端
MODULE: client/core
OWNER: Codex / WorkBuddy Agent 实施；@技术总监 二次验收；本地 MinGW MSYS2 g++ + vcpkg 编译验证
STATUS: PENDING
DEPENDENCIES: TASK-005
---

# TASK-034 · Client Core

> 本文件由 `tools/gen/build_tasks.py` 从 `tools/gen/data_*.py` 生成，**禁止手工编辑**。
> 需要改动请修改数据源后重新生成：`python tools/gen/build_tasks.py`

| 字段 | 值 |
|---|---|
| TASK-ID | `TASK-034` |
| NAME | Client Core |
| PHASE | Phase 8 · 客户端 |
| MODULE | `client/core` |
| OWNER | Codex / WorkBuddy Agent 实施；@技术总监 二次验收；本地 MinGW MSYS2 g++ + vcpkg 编译验证 |
| STATUS | **PENDING** |
| DEPENDENCIES | `TASK-005` |

---

> **✅ 前置已打通（2026-09-13）：RFC §9.8 触发条件已满足（TASK-030~033 / 037 / 039 / 040 / 041 全部 DONE），Godot 4.7.2 环境已纳入准备流程（下载与运行验证进行中，代理限速下预计约 45 分钟）。本任务解除冻结，可按 Godot 4.7.2 + 2D 规格开工；规格刷新（生成器重写）仍建议但不再作为硬阻塞。**
>
> **历史/背景**：原规格按「自研 C++ 客户端 / 自研渲染器 / 自研资源系统」编写，与 2026-08-29 批准的 Godot 4.7.x 路线（RFC §4.6/§6.1）及 2026-09-10 的 2D 优先约束（RFC §9）冲突，故于 2026-09-10 冻结。现服务端/游戏核心前置已全部完成、Godot 环境就绪，冻结解除。
>
>
> **不变项**（重写时必须保留）：协议契约（TASK-005 的 FlatBuffers schema）、AOI Delta 与快照格式、通过 GDExtension(C++) 下沉协议与热路径的策略、`client/{runtime,network,gameplay,ui,extensions}` 目录分层与单向依赖约束、以及「**逻辑层不得解算表现**」这条纪律（RFC §9.6）。

---

## 1. Objective

实现客户端核心：GameLoop / Network / Input / Scene / Entity / Protocol / Config。**客户端网络协议直接使用 TASK-005 定义的协议，不另起一套。**

## 2. Dependencies

### 2.1 前置任务

- `TASK-005` · Protocol Schema（Protobuf + FlatBuffers）

### 2.2 门禁规则

验收脚本会先执行 `require_tasks_done 005`：
任一前置任务的 `STATUS` 不是 `DONE`，脚本立即非零退出，**禁止越级实施**。

## 3. Module

`client/core`

## 4. State Owner（状态归属）

本地镜像状态由 ClientWorld 拥有（主线程）；网络接收缓冲由网络线程拥有，Poll 时交接所有权。禁止客户端判定任何权威状态。

> 硬约束：同一实时状态只能有一个权威写入者（见 PROJECT_REQUIREMENTS §10 / §12）。
> 跨模块写入必须走 Command，禁止直接改对方内存。

## 5. Input

TASK-005 Protocol Schema（唯一契约）；注意：客户端**不依赖**任何服务端模块，只通过 Protocol 衔接

## 6. Output

client/core 模块 + 连接网关跑通 + 客户端帧循环

## 7. Public Interface

```cpp
namespace mmo::client {
struct FrameContext { float dt_seconds; uint64_t frame_number; core::SteadyTime now;
                      core::Arena& frame_arena; };
class GameLoop { public:                     // 固定逻辑帧 + 可变渲染帧
  struct Config { uint32_t logic_hz{60}; uint32_t max_catchup{5}; bool vsync{true}; };
  core::Result<void> RegisterSystem(SystemPhase, std::function<void(const FrameContext&)>);
  core::Result<void> Run(); void Stop() noexcept;
  LoopStats Stats() const noexcept; };        // fps / logic_fps / frame_ms / longest_frame_ms
class NetClient { public:                    // 只实现客户端侧，不复用服务端传输模块
  core::Result<void> Connect(std::string_view addr, uint16_t port);
  core::Result<void> Send(uint32_t opcode, std::span<const uint8_t>);
  core::Result<void> Poll(std::vector<NetEvent>& out);   // 主线程驱动，禁止自带线程
  ConnectionState State() const noexcept; uint32_t RttMs() const noexcept; };
class ClientWorld { public:                  // 服务端状态的本地镜像 + 插值
  core::Result<void> ApplySnapshot(const protocol::SnapshotFrame&);  // FlatBuffers
  core::Result<void> Interpolate(const FrameContext&);               // 位置平滑
  entity::EntityId LocalPlayer() const noexcept; };
}
```

## 8. Data Model

**客户端架构分层（与服务端严格解耦）**

```
Client:  Core → Network → World → Renderer → Resource
Server:  Core → Communication → Gateway → GameNode → Gameplay → Data
                    └──── 只通过 Protocol 契约衔接 ────┘
```

**帧模型**：逻辑帧固定 60Hz（可配），渲染帧跟随 vsync。网络快照到达后进入**插值缓冲**（默认 100ms 延迟），平滑播放。
**状态镜像**：客户端只持有服务端下发的镜像快照，**不做权威判定**（伤害、命中均由服务端裁定）。

## 9. Thread Model

主线程：GameLoop + 输入；网络线程：只做收发搬运（**不解析业务**）；渲染线程：TASK-035 引入。跨线程只传不可变快照，禁止共享可变 Entity。

## 10. Hot Path

**YES** （帧循环与插值在客户端热路径）

本任务位于 Tick 热路径内，禁止：MySQL / Redis 同步访问 / 同步 gRPC / Kafka 同步访问 / 文件 IO / 网络阻塞 IO / 大规模内存分配（见 PROJECT_REQUIREMENTS §14）。

## 11. External IO

**YES** （网络与配置）

所有外部 IO 必须异步化，禁止出现在 Tick 内。

## 12. Network RPC

**NO** （用自定义二进制协议，不是 gRPC）


## 13. Persistence

**NO**


## 14. Files

- client/core/include/mmo/client/
- client/core/src/
- client/core/tests/
- client/core/docs/
- config/client/

## 15. Implementation Steps

1. 实现 game_loop.h/.cpp：固定逻辑步长 + CatchUp 限幅（max_catchup=5），帧耗时统计（最长帧、P95 帧）
2. 实现 net_client.h/.cpp：基于 TASK-005 协议的客户端收发，主线程 Poll（网络线程只搬运）
3. 实现连接管理：连接/重连/心跳/超时，状态机 Connecting→Connected→Reconnecting→Disconnected
4. 实现 protocol 复用：直接链接 protocol 模块的编解码库（Protobuf + FlatBuffers），**禁止重写一套**
5. 实现 input.h：输入采集与映射（键鼠/手柄抽象），输入只在逻辑帧开始采样一次
6. 实现 client_world.h/.cpp：快照应用 + 实体镜像 + 位置插值（100ms 缓冲，防抖动）
7. 实现插值与外推：位置用线性插值，短时丢包用外推（上限 200ms，超出则冻结）
8. 实现 config：客户端配置（config/client/*.json）—— 分辨率、帧率上限、网络参数、画质档位占位
9. 写测试：帧循环精度（60Hz 跑 10 秒误差 < 100ms）；插值正确性（两点之间位置线性）；断连重连；协议往返（与测试用服务端替身）
10. 写集成测试：起一个协议替身服务端，客户端连接 → 登录 → 收到快照 → 客户端渲染数据可用

## 16. Unit Test

GameLoop 帧精度与 CatchUp 限幅；NetClient 状态机；协议编解码往返（复用 protocol）；插值/外推计算；输入采样；配置加载

## 17. Integration Test

客户端连接测试替身服务端：完成连接 → 心跳 → 接收 100 帧快照 → 本地镜像实体数与服务端一致 → 插值后位置连续无跳变；中途断网 3 秒 → 自动重连 → 状态恢复

## 18. Benchmark

bin/client_bench：`frame_ms_p95=` / `snapshot_apply_ns=` / `interpolate_ns_per_entity=` / `mem_bytes_client_base=` / `net_poll_ns=`

## 19. Failure Test

服务端不可达：连接失败并返回明确错误，不崩溃、不无限阻塞；网络中断：转 Reconnecting 并按退避重连，本地画面冻结而非崩溃；收到损坏快照：校验失败丢弃该帧并使用上一帧（不崩溃）；帧耗时突增（如 GC/磁盘）：CatchUp 限幅，不产生死亡螺旋；收到未知 opcode：记录并跳过，不断开连接

## 20. Acceptance Criteria

1. **客户端协议直接复用 TASK-005**（grep：client 目录无独立的消息头定义）
2. **客户端不依赖任何服务端模块**（链接检查：client 目标不 link server 库）
3. 帧循环 60Hz 精度误差 < 100ms/10 秒，CatchUp 限幅生效
4. 连接 → 登录 → 收快照 → 插值全链路跑通（集成测试）
5. 断网重连可用，画面冻结而非崩溃
6. 损坏快照被丢弃且不崩溃
7. Debug / Release 双构建通过，ctest -R Client 全绿

以上每一条都必须在本地真实执行并留证；**任一条不满足即判定本任务未完成**，禁止进入下一个 TASK。

## 21. Forbidden

- 禁止客户端另起一套消息协议（必须复用 protocol 模块）
- 禁止客户端依赖服务端模块代码（只通过 Protocol 契约）
- 禁止客户端做权威判定（伤害/命中/掉落均由服务端裁定）
- 禁止网络线程解析业务包（只搬运）
- 禁止无 CatchUp 限幅的帧循环（死亡螺旋）
- 禁止损坏数据导致客户端崩溃

> 统一边界红线（全任务适用，详见 §27.3）：禁止扩散到他人 `module` 子树；下游禁止 `#include` 本任务 `src/`；禁止访问依赖模块内部数据；禁止在 `STATUS: DONE` 后静默改接口签名；禁止循环依赖。

## 22. Performance Expectation

逻辑帧 60Hz 稳定（P95 帧 < 16.6ms）；快照应用 1000 实体 < 1ms；插值 < 50ns/实体；客户端基础内存 < 150MB；网络 Poll < 100us。

## 23. Deliverables

- client/core/include/mmo/client/game_loop.h
- client/core/include/mmo/client/net_client.h
- client/core/include/mmo/client/client_world.h
- client/core/src/*.cpp
- client/core/tests/*
- config/client/*.json
- client/core/docs/INTERFACE.md
- client/core/docs/README.md

## 24. Verification Script（本地验收）

**验收脚本**：`scripts/verify/task-034.sh`（由生成器产出，禁止手工编辑）

```bash
# 默认 Release；可指定 Debug：BUILD_TYPE=Debug bash scripts/verify/task-034.sh
bash scripts/verify/task-034.sh
```

脚本执行的检查项：

1. 前置任务门禁：`require_tasks_done 005`
2. 交付物存在性检查（6 项）
3. CMake configure + 编译（Debug + Release 双构建）
4. ctest 过滤执行：`-R Client`
5. Benchmark 执行：`bin/client_bench --frames 600`
6. 性能阈值断言：`bench/client.txt` 中 `frame_ms_p95` ≤ `16.6`
7. 性能阈值断言：`bench/client.txt` 中 `mem_bytes_client_base` ≤ `157286400`

脚本遵循 `set -euo pipefail`：任一步失败即非零退出，**不存在「警告通过」**。
脚本只报告真实执行结果，禁止兜底伪造 PASS；指标缺失直接判失败，禁止用估算值代替。

## 25. Git Commit

**必须先通过验收脚本（退出码 0），才允许提交。**

```bash
# 1) 把本任务标记为 DONE（脚本会校验 STATUS 流转合法）
bash scripts/task-done.sh TASK-034

# 2) 提交：Conventional Commits，scope 用模块名
git add -A
git commit -F - <<'EOF'
feat(client): Client Core

- 实现要点：（填写本任务实际落地的内容，禁止复制 Objective）
- 实测数字：（粘贴 scripts/verify/task-034.sh 的真实输出，禁止写「性能良好」）

Refs: TASK-034
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
3. 查依赖：确认 TASK-005 均已 `STATUS: DONE`，否则停止并报告。
4. 查现状：grep 现有代码，确认要改的文件与符号真实存在，禁止凭空假设。
5. 守范围：只改本任务 §14 Files 范围内的文件；发现范围外问题只记录不修改。
6. 做实现：按 §15 Implementation Steps 顺序落地，每步可独立编译。
7. 本地编译：MinGW MSYS2 g++ + vcpkg manifest mode（baseline `aae277ac`），Debug 与 Release 都要过。
8. 跑单测：§16 Unit Test 全绿，新增代码必须带测试。
9. 跑集成：§17 Integration Test 全绿。
10. 跑 Benchmark：§18 真实执行，输出机器可读的 `key=value`，禁止估算。
11. 出结果：把实测数字写入 §22 对应的报告文件，不达标如实记录。
12. 跑验收脚本：`bash scripts/verify/task-034.sh` 退出码 0 后，才执行 §25 提交。

> **门禁**：本任务未通过 §20 Acceptance Criteria 与 §24 验收脚本，禁止进入下一个 TASK。

## 27. 接口契约、模块边界与扩展性

本节是**防任务间交付相互干扰 + 保框架可扩展**的统一契约，所有任务适用。

### 27.1 本任务导出的接口（冻结后不可破坏性变更）

见 §7 Public Interface。导出头只放在本任务 `include/` 下，签名一旦 `STATUS: DONE` 即视为契约冻结，下游依赖它；破坏性变更须走 `version` + 兼容性评估。

### 27.2 本任务消费的上游接口（来自前置任务，禁止绕过）

- `TASK-005` · `protocol`：消费其 `include/` 下公开接口（详见该任务 §7 Public Interface），禁止 `#include` 其 `src/`

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
