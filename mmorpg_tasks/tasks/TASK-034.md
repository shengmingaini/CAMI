---
TASK-ID: TASK-034
NAME: Client Core
PHASE: Phase 8 · 客户端
MODULE: client/runtime
OWNER: Codex / WorkBuddy Agent 实施；@技术总监 二次验收；本地 Godot 4.7.2 构建验证（require_godot 门禁）+ GDExtension(C++) scons 编译
STATUS: DONE
DONE-DATE: 2026-09-14
DEPENDENCIES: TASK-005
---

# TASK-034 · Client Core（Godot 4.7.2 · 2.5D）

> **本任务书按 `docs/client-spec-2.5d.md` §8 + §12 安全刷新（2026-09-14），取代原「自研 C++ 客户端 / 自研主循环」旧框架。**
> ⚠ **禁止运行 `python tools/gen/build_tasks.py`**：生成器会把全部 42 份任务书 `STATUS` 硬写为 `PENDING`，清空已完成的 39 个 DONE 台账。本刷新只手工改本任务书，STATUS 保持 `PENDING` 不变。

| 字段 | 值 |
|---|---|
| TASK-ID | `TASK-034` |
| NAME | Client Core |
| PHASE | Phase 8 · 客户端 |
| MODULE | `client/runtime` |
| OWNER | Codex / WorkBuddy Agent 实施；@技术总监 二次验收；本地 Godot 4.7.2 构建验证（require_godot 门禁）+ GDExtension(C++) scons 编译 |
| STATUS | **PENDING** |
| DEPENDENCIES | `TASK-005` |

---

> **✅ 前置已打通（2026-09-13）：RFC §9.8 触发条件已满足（TASK-030~033 / 037 / 039 / 040 / 041 全部 DONE），Godot 4.7.2 已解压核验（`4.7.2.stable.official.ed1daf0bf`，路径 `F:\AI\tools\godot\4.7.2\Godot_v4.7.2-stable_win64.exe`）。本任务解除冻结，按 `docs/client-spec-2.5d.md` 2.5D 规格开工；规格已刷新，无需再等生成器重写。**
>
> **历史/背景**：原规格按「自研 C++ 客户端 / 自研渲染器 / 自研资源系统」编写，与 2026-08-29 批准的 Godot 4.7.x 路线（RFC §4.6/§6.1）、2026-09-10 的 2D 优先约束（RFC §9）以及 2026-09-14 升级的 2.5D 规格（§9）均冲突，故于 2026-09-10 冻结。现服务端/游戏核心前置已全部完成、Godot 环境就绪、2.5D 规格已定稿，冻结解除。
>
> **不变项**（重写时必须保留）：协议契约（TASK-005 的 FlatBuffers schema）、AOI Delta 与快照格式、通过 GDExtension(C++) 下沉协议与热路径的策略、`client/{runtime,network,gameplay,ui,extensions}` 目录分层与单向依赖约束、以及「**逻辑层不得解算表现**」这条纪律（RFC §9.6 / 规格书 §1 末尾）。

---

## 1. Objective

实现客户端核心运行时（Godot 4.7.2 工程骨架 + 应用生命周期 / 场景树管理 / 目录分层）：`GameLoop`（autoload，固定逻辑帧 60Hz + vsync 渲染帧）、`NetClient`（主线程 Poll，不复用服务端传输模块）、`ClientWorld`（服务端状态本地镜像 + 插值）、`InputManager`、客户端配置。**客户端网络协议直接使用 TASK-005 定义的协议（经 GDExtension(C++) 协议编解码绑定），不另起一套。** 取代原「自研主循环 + 自研窗口管理」——客户端主循环改用 Godot `_process` / `_physics_process`。

## 2. Dependencies

### 2.1 前置任务

- `TASK-005` · Protocol Schema（Protobuf + FlatBuffers）

### 2.2 门禁规则

验收脚本会先执行 `require_tasks_done 005`：
任一前置任务的 `STATUS` 不是 `DONE`，脚本立即非零退出，**禁止越级实施**。

## 3. Module

`client/runtime`（Godot 工程内 `client/runtime/` 目录；对应规格书 §7 目录分层中的「核心运行时」子树。网络/世界镜像/输入亦归本模块，拆分到 `client/network` 等子目录由后续任务承接，本任务只落地 runtime + network 基础）。

## 4. State Owner（状态归属）

本地镜像状态由 `ClientWorld` 拥有（Godot 主线程）；网络接收缓冲由 `NetClient` 拥有，Poll 时交接所有权。禁止客户端判定任何权威状态。

> 硬约束：同一实时状态只能有一个权威写入者（见 PROJECT_REQUIREMENTS §10 / §12）。
> 跨模块写入必须走 Command，禁止直接改对方内存。

## 5. Input

TASK-005 Protocol Schema（唯一契约）；注意：客户端**不依赖**任何服务端模块，只通过 Protocol 衔接（GDExtension 绑定暴露解码接口，GDScript 调用）。

## 6. Output

`client/runtime` + `client/network` 模块 + 连接网关跑通 + 客户端帧循环 + Godot 工程可 `_physics_process` 驱动逻辑帧。

## 7. Public Interface

```gdscript
# client/runtime/autoload/game_loop.gd  —— GameLoop（autoload，固定逻辑帧 + 可变渲染帧）
class_name GameLoop
extends Node
const LOGIC_HZ     : int = 60      # 固定逻辑帧
const MAX_CATCHUP  : int = 5       # CatchUp 限幅，防死亡螺旋
const USE_VSYNC    : bool = true
signal frame_start(ctx: Dictionary) # {dt_seconds, frame_number, now_ms}
func _physics_process(delta: float) -> void:   # 固定步长累加，超时 CatchUp 限幅
	pass
func stats() -> Dictionary:  # {fps, logic_fps, frame_ms, longest_frame_ms}

# client/network/net_client.gd  —— NetClient（autoload，仅客户端侧，主线程 Poll）
class_name NetClient
extends Node
func connect_to(addr: String, port: int) -> Result
func poll() -> Array        # 返回 Array[NetEvent]，主线程驱动，禁止自带线程
func send(opcode: int, payload: PackedByteArray) -> Result
func state() -> int         # Connecting/Connected/Reconnecting/Disconnected
func rtt_ms() -> int

# client/runtime/autoload/client_world.gd  —— ClientWorld（服务端镜像 + 插值）
class_name ClientWorld
extends Node
# 入参 fb 为 FlatBuffers 快照字节，经 GDExtension 协议绑定解码
func apply_snapshot(fb: PackedByteArray) -> Result
func interpolate(dt: float) -> void          # 100ms 插值缓冲平滑播放
func local_player_id() -> int

# GDExtension(C++) 绑定（client/extensions/protocol_codec）：复用 TASK-005 编解码
#   decode_snapshot(PackedByteArray) -> Dictionary(SnapshotFrame)
#   encode_command(opcode:int, payload:Dictionary) -> PackedByteArray
#   禁止在 GDScript 层重写协议编解码
```

## 8. Data Model

**客户端架构分层（与服务端严格解耦，Godot 工程内）**

```
Client (Godot 工程):
  runtime  → network → world → (renderer TASK-035) → (resource TASK-036)
  ui / gameplay / extensions 由后续任务承接
Server:
  Core → Communication → Gateway → GameNode → Gameplay → Data
            └──── 只通过 Protocol 契约衔接（GDExtension 协议绑定） ────┘
```

**帧模型**：逻辑帧固定 60Hz（`GameLoop` autoload 在 `_physics_process` 累加固定步长），渲染帧跟随 vsync。网络快照到达后进入**插值缓冲**（默认 100ms 延迟），平滑播放。
**状态镜像**：客户端只持有服务端下发的镜像快照，**不做权威判定**（伤害、命中均由服务端裁定）。

## 9. Thread Model

Godot 主线程：`GameLoop`（`_process`/`_physics_process`）+ `InputManager` + `ClientWorld` 插值。`NetClient` 仅做收发搬运，由 Godot `StreamPeerTCP` 信号驱动，主线程 `poll()` 取事件，**不自建线程、不解析业务**。GDExtension 协议解码在主线程同步调用（热路径下沉 C++，复用 TASK-005）。渲染交由 Godot 内部渲染线程（Compatibility 单档），表现层代码不直接持有渲染线程。跨线程只传不可变快照，禁止共享可变 Entity。

> 与规格书 §6 一致：位置/朝向/状态由服务端数据驱动，客户端仅「翻译为画面」，逻辑层不得解算表现。

## 10. Hot Path

**YES** （帧循环与插值在客户端热路径）

本任务位于 Tick 热路径内，禁止：MySQL / Redis 同步访问 / 同步 gRPC / Kafka 同步访问 / 文件 IO / 网络阻塞 IO / 大规模内存分配（见 PROJECT_REQUIREMENTS §14）。Godot 内置对象分配亦需克制（插值缓冲预分配，避免每帧 new）。

## 11. External IO

**YES** （网络与配置）

所有外部 IO 必须异步化，禁止出现在 `_physics_process` 内（网络收发由信号 + 主线程 poll 完成，配置加载走 `ResourceLoader.load` 异步或启动期一次性加载）。

## 12. Network RPC

**NO** （用自定义二进制协议，不是 gRPC）

## 13. Persistence

**NO**

## 14. Files

- client/project.godot
- client/runtime/autoload/game_loop.gd
- client/runtime/autoload/client_world.gd
- client/runtime/autoload/input_manager.gd
- client/runtime/autoload/config.gd
- client/network/net_client.gd
- client/extensions/protocol_codec/（GDExtension C++ 绑定，复用 TASK-005 编解码）
- client/runtime/tests/
- client/runtime/docs/
- config/client/*.json

## 15. Implementation Steps

1. 建 Godot 4.7.2 工程骨架：`client/project.godot`、autoload 注册（`GameLoop` / `NetClient` / `ClientWorld` / `InputManager` / `Config`）、场景树根节点与子场景挂载约定。
2. 实现 `GameLoop`：`_physics_process` 固定步长累加 + CatchUp 限幅（max_catchup=5）+ 帧耗时统计（最长帧、P95）；vsync 由工程设置控制。
3. 实现 `NetClient`：基于 TASK-005 协议的客户端收发（StreamPeerTCP），主线程 `poll()` 取事件，状态机 Connecting→Connected→Reconnecting→Disconnected。
4. 实现 GDExtension 协议绑定 `protocol_codec`：封装 TASK-005 的 FlatBuffers 编解码 C++，暴露 `decode_snapshot` / `encode_command` 给 GDScript；**禁止在 GDScript 重写协议**。
5. 实现 `ClientWorld.apply_snapshot`：经 GDExtension 解码 → 实体镜像写入（不可变快照交接）；`interpolate`：100ms 缓冲线性插值，短时丢包外推（上限 200ms，超出冻结）。
6. 实现 `InputManager`：键鼠/手柄抽象，输入只在逻辑帧开始采样一次；屏幕→世界射线（相机 `intersect_ray`，供后续拾取）。
7. 实现 `Config`：客户端配置（`config/client/*.json`）—— 分辨率、帧率上限、网络参数、画质档位占位（三档预设由 TASK-036 填充）。
8. 写测试：帧循环精度（60Hz 跑 10 秒误差 < 100ms）；插值正确性（两点之间位置线性）；断连重连状态机；协议往返（与测试用服务端替身，经 GDExtension）；输入采样。
9. 写集成测试：起协议替身服务端，客户端连接 → 登录 → 收快照 → `ClientWorld` 实体数一致 → 插值后位置连续无跳变；中途断网 3 秒 → 自动重连 → 状态恢复。

## 16. Unit Test

`GameLoop` 帧精度与 CatchUp 限幅；`NetClient` 状态机；GDExtension 协议编解码往返（复用 TASK-005）；插值/外推计算；输入采样；配置加载。

## 17. Integration Test

客户端连接测试替身服务端：完成连接 → 心跳 → 接收 100 帧快照 → 本地镜像实体数与服务端一致 → 插值后位置连续无跳变；中途断网 3 秒 → 自动重连 → 状态恢复。

## 18. Benchmark

`godot --headless --path client --benchmark-frame`（或 `godot_run_tests` 封装）：`frame_ms_p95=` / `snapshot_apply_ns=` / `interpolate_ns_per_entity=` / `mem_bytes_client_base=` / `net_poll_ns=`

## 19. Failure Test

服务端不可达：连接失败并返回明确错误，不崩溃、不无限阻塞；网络中断：转 Reconnecting 并按退避重连，本地画面冻结而非崩溃；收到损坏快照：GDExtension 校验失败丢弃该帧并使用上一帧（不崩溃）；帧耗时突增：CatchUp 限幅，不产生死亡螺旋；收到未知 opcode：记录并跳过，不断开连接。

## 20. Acceptance Criteria

1. **客户端协议直接复用 TASK-005**（经 GDExtension 绑定；grep 确认 `client/` 无独立协议重写）。
2. **客户端不依赖任何服务端模块**（构建检查：client 目标不 link server 库；GDExtension 只链接 protocol 编解码）。
3. 帧循环 60Hz 精度误差 < 100ms/10 秒，CatchUp 限幅生效（`GameLoop` 单测）。
4. 连接 → 登录 → 收快照 → 插值全链路跑通（集成测试）。
5. 断网重连可用，画面冻结而非崩溃。
6. 损坏快照被丢弃且不崩溃。
7. `require_godot` 门禁通过：Godot 4.7.2 可执行存在且版本匹配；工程 `_physics_process` 驱动逻辑帧；`godot_run_tests` 全绿。

以上每一条都必须在本地真实执行并留证；**任一条不满足即判定本任务未完成**，禁止进入下一个 TASK。

## 21. Forbidden

- 禁止客户端另起一套消息协议（必须复用 protocol 模块 / GDExtension 绑定）
- 禁止客户端依赖服务端模块代码（只通过 Protocol 契约）
- 禁止客户端做权威判定（伤害/命中/掉落均由服务端裁定）
- 禁止 `NetClient` 自建线程解析业务包（只搬运，主线程 Poll）
- 禁止无 CatchUp 限幅的帧循环（死亡螺旋）
- 禁止损坏数据导致客户端崩溃
- 禁止逻辑层解算表现（位置/朝向/状态由服务端数据驱动，规格书 §1）

> 统一边界红线（全任务适用，详见 §27.3）：禁止扩散到他人 `module` 子树；下游禁止反向 import 上层目录；禁止访问依赖模块内部数据；禁止在 `STATUS: DONE` 后静默改接口签名；禁止循环依赖。

## 22. Performance Expectation

逻辑帧 60Hz 稳定（P95 帧 < 16.6ms）；快照应用 1000 实体 < 1ms；插值 < 50ns/实体；客户端基础内存 < 150MB；网络 Poll < 100us。

## 23. Deliverables

- client/project.godot
- client/runtime/autoload/game_loop.gd
- client/runtime/autoload/client_world.gd
- client/runtime/autoload/input_manager.gd
- client/runtime/autoload/config.gd
- client/network/net_client.gd
- client/extensions/protocol_codec/（GDExtension C++ 绑定）
- client/runtime/tests/*
- config/client/*.json
- client/runtime/docs/INTERFACE.md
- client/runtime/docs/README.md

## 24. Verification Script（本地验收）

**验收脚本**：`scripts/verify/task-034.sh`（手写，执行 `require_godot` 门禁 + Godot 工程校验）

```bash
# 默认 Release；可指定 Debug：BUILD_TYPE=Debug bash scripts/verify/task-034.sh
bash scripts/verify/task-034.sh
```

脚本执行的检查项：

1. 前置任务门禁：`require_tasks_done 005`
2. `require_godot`：探测 Godot 4.7.2 可执行（版本匹配 `4.7.2.stable.official.ed1daf0bf`），未命中即非零退出
3. 交付物存在性检查（6 项）
4. GDExtension 编译：scons 编译 `client/extensions/protocol_codec`（Debug + Release）
5. Godot 工程可加载：`godot --headless --path client --check-only` 退出码 0
6. 测试执行：`godot_run_tests` 或 `godot --headless --path client --run-tests`（过滤 Client）
7. Benchmark 执行：`frame_ms_p95` 阈值断言 ≤ `16.6`
8. 性能阈值断言：`mem_bytes_client_base` ≤ `157286400`

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
feat(client): Client Core (Godot 4.7.2 · 2.5D)

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

1. 读规范：先读 `PROJECT_REQUIREMENTS.md`、`docs/client-spec-2.5d.md` 与本任务涉及章节，架构冻结，不得自行推翻。
2. 读任务：完整读完本文件全部章节再动手，禁止只看 Objective 就开始写。
3. 查依赖：确认 TASK-005 均已 `STATUS: DONE`，否则停止并报告。
4. 查现状：grep 现有代码，确认要改的文件与符号真实存在，禁止凭空假设。
5. 守范围：只改本任务 §14 Files 范围内的文件；发现范围外问题只记录不修改。
6. 做实现：按 §15 Implementation Steps 顺序落地，每步可独立编译/运行。
7. 本地构建：`require_godot` 门禁 + GDExtension scons 编译（Debug 与 Release 都要过）；Godot 工程 `_headless --check-only` 通过。
8. 跑单测：§16 Unit Test 全绿，新增代码必须带测试。
9. 跑集成：§17 Integration Test 全绿。
10. 跑 Benchmark：§18 真实执行，输出机器可读的 `key=value`，禁止估算。
11. 出结果：把实测数字写入 §22 对应的报告文件，不达标如实记录。
12. 跑验收脚本：`bash scripts/verify/task-034.sh` 退出码 0 后，才执行 §25 提交。

> **门禁**：本任务未通过 §20 Acceptance Criteria 与 §24 验收脚本，禁止进入下一个 TASK。

## 27. 接口契约、模块边界与扩展性

本节是**防任务间交付相互干扰 + 保框架可扩展**的统一契约，所有任务适用。

### 27.1 本任务导出的接口（冻结后不可破坏性变更）

见 §7 Public Interface。Godot autoload 类名（`GameLoop` / `NetClient` / `ClientWorld` / `InputManager` / `Config`）与 GDExtension 绑定签名（`decode_snapshot` / `encode_command`）一旦 `STATUS: DONE` 即视为契约冻结，下游依赖它；破坏性变更须走 `version` + 兼容性评估。

### 27.2 本任务消费的上游接口（来自前置任务，禁止绕过）

- `TASK-005` · `protocol`：经 GDExtension 绑定消费其编解码（详见该任务 §7 Public Interface），禁止在 GDScript 层重写协议。

### 27.3 模块边界红线（全任务统一）

- 模块 ≠ 进程：本任务代码只落在自身 `module` 子树（`client/runtime/` + `client/network/` + `client/extensions/protocol_codec/`），禁止扩散到其它任务拥有的目录。
- 下游只能通过本任务 autoload / GDExtension 暴露的**公开接口**调用，禁止反向 import 上层目录或访问内部实现（验收脚本静态扫描 `client/ui/`、`client/gameplay/` 不得反向依赖 runtime 内部）。
- 本任务只调用依赖模块**声明**的接口，禁止访问其内部数据。
- 接口在 `STATUS: DONE` 之后变更必须走 `version` 字段 + 兼容性评估，禁止静默改签名导致下游编译失败。
- 依赖方向单向（runtime → network → world → renderer/resource，且 `client/ui/`、`client/gameplay/` 不得反向依赖），禁止循环依赖。

### 27.4 扩展性约束（可扩展框架兼容性）

- 新增同类能力（新 Command / 新 Event / 新场景类型 / 新模块）必须走**注册表 / ID 段**机制，禁止在 `match`/`switch` 里硬编码穷举。
- 跨模块扩展点统一用 Godot 信号 / 资源（Resource）/ 抽象接口，新增实现**不得修改既有任务文件**。
- 协议 / 接口变更必须带 `version` 字段并向下兼容，旧客户端 / 旧模块不得因此断连或编译失败。
- 所有模块遵循统一目录模板（`client/{runtime,network,gameplay,ui,extensions}` + `docs/`），新增模块不得例外。

> 模块归属表（谁拥有哪棵子树）：本任务的 `module` 字段即其独占目录；
> 其它任务的 `module` 字段不得被本任务写入。统一模块模板见 `DEVELOPMENT.md` / 根规范 §6。

## 28. 变更记录

| 日期 | 变更 |
|---|---|
| 2026-08-29 | 方案 A 原地补齐：由 `tools/gen/build_tasks.py` 从结构化数据源重新生成，补齐 State Owner / 验收脚本 / STATUS 门禁 / Git Commit 规范 |
| 2026-09-14 | **2.5D 刷新**：按 `docs/client-spec-2.5d.md` §8 重写——由「自研 C++ 客户端 / 自研主循环」改为 Godot 4.7.2 工程骨架 + `GameLoop` autoload（`_process`/`_physics_process`，60Hz，max_catchup=5）取代自研主循环；NetClient/ClientWorld/输入/配置保留；协议改经 GDExtension(C++) 绑定复用 TASK-005；构建门禁改 `require_godot`；STATUS 保持 PENDING。禁止运行生成器（会重置 STATUS） |
