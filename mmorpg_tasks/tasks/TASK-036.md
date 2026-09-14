---
TASK-ID: TASK-036
NAME: Resource / Low Spec System (2.5D)
PHASE: Phase 8 · 客户端
MODULE: client/resource
OWNER: Codex / WorkBuddy Agent 实施；@技术总监 二次验收；本地 Godot 4.7.2 构建验证（require_godot 门禁）+ GDExtension(C++) scons 编译
STATUS: DONE
DONE-DATE: 2026-09-14
DEPENDENCIES: TASK-034, TASK-035
---

# TASK-036 · Resource / Low Spec System（Godot 4.7.2 · 2.5D）

> **本任务书按 `docs/client-spec-2.5d.md` §8 + §12 安全刷新（2026-09-14），取代原「自研资源系统 / 纯 2D 分块流式」旧框架。**
> ⚠ **禁止运行 `python tools/gen/build_tasks.py`**：生成器会把全部 42 份任务书 `STATUS` 硬写为 `PENDING`，清空已完成的 39 个 DONE 台账。本刷新只手工改本任务书，STATUS 保持 `PENDING` 不变。

| 字段 | 值 |
|---|---|
| TASK-ID | `TASK-036` |
| NAME | Resource / Low Spec System (2.5D) |
| PHASE | Phase 8 · 客户端 |
| MODULE | `client/resource` |
| OWNER | Codex / WorkBuddy Agent 实施；@技术总监 二次验收；本地 Godot 4.7.2 构建验证（require_godot 门禁）+ GDExtension(C++) scons 编译 |
| STATUS | **PENDING** |
| DEPENDENCIES | `TASK-034`, `TASK-035` |

---

> **✅ 前置已打通（2026-09-13）：RFC §9.8 触发条件已满足（TASK-030~033 / 037 / 039 / 040 / 041 全部 DONE），Godot 4.7.2 已解压核验（`4.7.2.stable.official.ed1daf0bf`）。TASK-034/035 解除冻结后本任务即具备开工条件，按 `docs/client-spec-2.5d.md` 2.5D 规格实施。**
>
> **历史/背景**：原规格按「自研资源系统 / 纯 2D 分块流式」编写，与 2026-08-29 批准的 Godot 4.7.x 路线（RFC §4.6/§6.1）及 2026-09-14 升级的 2.5D 规格（§9）冲突，故于 2026-09-10 冻结。现 Godot 路线 + 2.5D 规格已定稿，冻结解除。
>
> **不变项**（重写时必须保留）：协议契约（TASK-005 的 FlatBuffers schema）、AOI Delta 与快照格式、通过 GDExtension(C++) 下沉协议与热路径的策略、`client/{runtime,network,gameplay,ui,extensions}` 目录分层与单向依赖约束、以及「**逻辑层不得解算表现**」这条纪律（RFC §9.6 / 规格书 §1 末尾）。

---

## 1. Objective

这是「老电脑能跑」的核心任务：资源管线改为 **Godot `.import` + 图集（`AtlasTexture` / `TextureAtlas`）+ 程序化网格缓存**；分块流式（Chunk Streaming）改为 **3D 世界分块**（九宫格 / 半径加载，进入异步加载、超出延迟 5s 释放防抖）。支持 Low / Medium / High 三档（配置化，消费 `QualityPreset`）。三档预算增加「三角面预算 / 精灵图集分辨率 / 地面网格密度」。**地图不能全部常驻内存：只保留 Current Chunk + Nearby Chunk，远处自动释放。**

## 2. Dependencies

### 2.1 前置任务

- `TASK-034` · Client Core
- `TASK-035` · Renderer (2.5D)

### 2.2 门禁规则

验收脚本会先执行 `require_tasks_done 034 035`：
任一前置任务的 `STATUS` 不是 `DONE`，脚本立即非零退出，**禁止越级实施**。

## 3. Module

`client/resource`（Godot 工程内 `client/resource/` 目录；资源加载/缓存/画质预设 + 3D 世界分块流式 + 程序化网格缓存 + 低配容量采样）。

## 4. State Owner（状态归属）

资源句柄与缓存表由 `ResourceManager` 拥有（主线程 + 引用计数）；已加载 Chunk 集合由 `SceneStreamer` 拥有。释放必须等渲染线程无引用（Godot `Resource` 引用计数自动管理，禁止手动 `free` 正在渲染的资源）。

> 硬约束：同一实时状态只能有一个权威写入者（见 PROJECT_REQUIREMENTS §10 / §12）。资源生命周期由本模块独占管理。

## 5. Input

TASK-034 `ClientWorld`（玩家位置，驱动分块加载）；TASK-035 `RenderStats` / `QualityPreset`（画质切换信号、LOD 距离）；低配红线（4 核 / 4GB RAM / 1GB VRAM，开发目标非承诺）。

## 6. Output

`client/resource` 模块 + 流式加载 + 三档配置 + 资源占用报告（含三角面/图集分辨率/地面网格密度预算）。

## 7. Public Interface

```gdscript
# client/resource/resource_manager.gd  —— 资源加载/缓存/画质
class_name ResourceManager
extends Node
func init(preset: Dictionary) -> void                 # 三档预设（QualityPreset）
func load_async(uri: String, type: int) -> Resource    # 异步加载，返回 Resource
func unload(res: Resource) -> void
func set_quality(level: int) -> void                   # 热切换 + 按新预算回收
func stats() -> Dictionary:  # {ram_mb, vram_mb, draw_calls, texture_mem_mb, mesh_mem_mb, ...}

# client/resource/scene_streamer.gd  —— 3D 世界分块流式
class_name SceneStreamer
extends Node
const CHUNK_SIZE       : float = 128.0
const UNLOAD_DELAY_SEC : float = 5.0                   # 超出卸载半径延迟 5s 释放防抖
func update(player_pos: Vector3) -> void              # 主线程每帧，按帧预算限流
func force_unload_all() -> void
func stats() -> Dictionary:  # {loaded_chunks, pending, last_load_ms}

# client/resource/proc_mesh_cache.gd  —— 程序化网格缓存（地面/地形，无 DCC 建模）
class_name ProcMeshCache
extends Node
func get_or_build(chunk_key: Vector2i, hm: Image) -> ArrayMesh  # 缓存命中复用
func set_lod_density(level: int) -> void

# client/resource/quality_preset.gd  —— 三档预设（配置化，禁止硬编码）
class_name QualityPreset
extends Resource
# 字段见 §8 三档表（纹理上限/地面网格密度/LOD/视距/区块半径/纹理预算/网格预算/...）
```

## 8. Data Model

**三档预设（配置化，`config/client/quality.json`，禁止硬编码）**

| 项 | Low | Medium | High |
|---|---|---|---|
| 纹理上限（图集/贴图） | 512 | 1024 | 2048 |
| 地面网格密度 | 低（大格子） | 中 | 高（小格子 + 细节） |
| LOD / 远精灵降级 | 激进 | 中 | 关 |
| 阴影 | 关 | 关 | 关（环境光近似） |
| 同屏实体 | 50 | 150 | 300 |
| 粒子数 | 200 | 1000 | 3000 |
| 网络更新率 | 10Hz | 20Hz | 30Hz |
| 视距 | 80m | 150m | 250m |
| 区块半径 | 1 | 2 | 3 |
| 纹理预算 | 256MB | 512MB | 1GB |
| 网格预算 | 128MB | 256MB | 512MB |
| **三角面预算（2.5D 新增）** | **< 300k** | — | — |
| **精灵图集分辨率（2.5D 新增）** | 512 | 1024 | 2048 |

**内存硬约束（Low 档目标）**：总 RAM < 1.5GB、VRAM < 1GB、Draw Call < 300、加载时间 < 15s。
**Chunk 策略（3D 世界分块）**：`Current Chunk + Nearby Chunk`（九宫格/半径），进入加载半径异步加载（Godot `ResourceLoader.load` 异步或 `Thread` 加载），超出卸载半径延迟 5 秒释放（防来回抖动）。

## 9. Thread Model

资源加载在 Worker 线程池（2~4 线程，按 CPU 核数；Godot `ResourceLoader` 后台线程或自建 `Thread`）；卸载在主线程安全点执行（Godot `Resource` 引用计数归零自动释放，渲染无引用时）；`SceneStreamer` 在主线程 `update`，按帧预算限流（每帧最多 1 个 chunk 加载完成回调）。

## 10. Hot Path

**YES** （每帧 Streaming 检查）

本任务位于 Tick 热路径内，禁止：MySQL / Redis 同步访问 / 同步 gRPC / Kafka 同步访问 / 文件 IO / 网络阻塞 IO / 大规模内存分配（见 PROJECT_REQUIREMENTS §14）。加载完成回调限流，禁止单帧堆积。

## 11. External IO

**YES** （磁盘/包体读取）

所有外部 IO 必须异步化（`ResourceLoader.load` 异步 / `Thread`），禁止出现在 `_process` 内。

## 12. Network RPC

**NO**

## 13. Persistence

**NO**

## 14. Files

- client/resource/resource_manager.gd
- client/resource/scene_streamer.gd
- client/resource/proc_mesh_cache.gd
- client/resource/quality_preset.gd
- client/resource/tests/
- client/resource/benchmark/
- config/client/quality.json
- tools/lowspec/profile.gd（或 .py 包装，采集 RAM/VRAM/FPS/DrawCall 曲线）

## 15. Implementation Steps

1. 定义 `quality_preset.gd` + `config/client/quality.json`：三档预设结构与配置加载（含 2.5D 新增字段：三角面预算 / 精灵图集分辨率 / 地面网格密度）。
2. 实现 `resource_manager.gd`：异步加载、引用计数、LRU 回收、预算超限强制回收（纹理/网格各自预算）。
3. 实现图集管线：Godot `.import` + `AtlasTexture` / `TextureAtlas`（8 向精灵图集、TileSet 纹理图集），按档位限制图集分辨率。
4. 实现 `proc_mesh_cache.gd`：程序化地面网格缓存（`MeshDataTool`/`ArrayMesh`），按 chunk_key 命中复用；网格密度随档位变化。
5. 实现 `scene_streamer.gd`：按玩家位置的九宫格/半径 Chunk 异步加载与延迟卸载（5s 防抖）。
6. 实现加载限流：每帧最多处理 N 个加载完成回调（防卡顿尖峰），pending 队列有上限。
7. 实现防抖动：Chunk 进入/离开边界加迟滞（hysteresis），快速来回移动不触发反复加载。
8. 实现画质热切换：切档后按新预算回收（先回收纹理，再回收网格），并重建 LOD 距离 / 网格密度。
9. 实现资源统计：`ram_mb` / `vram_mb` / `draw_calls` / 实体数 / 粒子数 / 加载队列长度 / 最近加载耗时。
10. 实现 LOD 联动：`SceneStreamer` 与 TASK-035 `LODManager` 共享 `QualityPreset`，距离阈值统一来源。
11. 写测试：LRU 回收；预算超限强制回收；Chunk 加载/卸载边界（含迟滞）；加载限流；画质切换后资源占用变化；异步加载不卡主线程。
12. 写**低配实测脚本 `tools/lowspec/profile.gd`**：采集 RAM/VRAM/FPS/DrawCall 曲线并出报告。

## 16. Unit Test

`QualityPreset` 加载与校验；`ResourceManager` 引用计数与 LRU；图集分辨率随档限制；`ProcMeshCache` 命中复用；Chunk 加载/卸载判定（含迟滞）；加载限流；画质切换后回收；统计字段。

## 17. Integration Test

**长距离移动测试**：玩家以最高速度在 3D 世界分块地图上跑 5 分钟，验证：常驻 Chunk 数稳定在配置值（不无限增长）、RAM 曲线平稳（不单调上升）、无卡顿尖峰（最长帧 < 100ms）、远处 Chunk 被正确释放；来回穿越 Chunk 边界 50 次，加载次数不爆炸（迟滞生效）；三角面预算随地面网格密度受控（Low < 300k）。

## 18. Benchmark

`godot --headless --path client --benchmark-resource` + `tools/lowspec/profile.gd`：`ram_mb=` / `vram_mb=` / `draw_calls=` / `load_ms_p95=` / `chunks_loaded=` / `cache_hit_rate=` / `fps_p95=` / `triangles=`

## 19. Failure Test

磁盘资源缺失：使用占位资源 + 告警，不崩溃；加载队列打满：丢弃最远 Chunk 请求并计数；显存不足：按 LRU 强制回收（先卸载远距离纹理/网格），若仍不足则拒绝加载并降级画质；玩家瞬移（跨 50 个 Chunk）：分批加载（限流），不产生单帧 10 秒卡顿；画质切到 Low 时显存未及时回落：强制回收并在 2 秒内达标；程序化网格生成失败：使用占位平面并告警。

## 20. Acceptance Criteria

1. **Current Chunk + Nearby Chunk 策略生效**（3D 世界分块），远处自动释放（集成测试：5 分钟跑图 RAM 平稳）
2. Low / Medium / High 三档全部可用且可热切换（消费 `QualityPreset`）
3. **Low 档 RAM < 1.5GB、VRAM < 1GB、Draw Call < 300**（实测，写入 PERFORMANCE.md）
4. **Low 档三角面 < 300k、精灵图集分辨率 ≤ 512**（2.5D 新增预算，实测）
5. Godot `.import` + `AtlasTexture`/`TextureAtlas` 管线生效（图集分辨率随档限制）
6. 程序化网格缓存命中复用（地面网格不重复生成）
7. 三类缓存各自独立预算，互不挤占（单测）
8. 来回穿越 Chunk 边界不触发加载风暴（迟滞生效）
9. 低配实测脚本可输出完整资源曲线报告
10. `require_godot` 门禁通过；Godot 工程可加载；测试全绿

以上每一条都必须在本地真实执行并留证；**任一条不满足即判定本任务未完成**，禁止进入下一个 TASK。

## 21. Forbidden

- 禁止地图全部常驻内存（必须 3D 世界分块流式）
- 禁止无预算上限的资源缓存（纹理/网格/图集各自预算）
- 禁止主线程同步加载资源
- 禁止画质切到 Low 后显存不回落
- 禁止硬编码画质参数（必须配置化，消费 `QualityPreset`）
- 禁止生产 3D 几何模型（地面用程序化网格，角色用 8 向精灵）
- 禁止逻辑层解算表现（位置/朝向/状态由服务端数据驱动，规格书 §1）
- 禁止在未实测前宣称支持 4核/4GB/1GB VRAM 具体硬件
- 禁止通过提高硬件要求来回避性能问题

> 统一边界红线（全任务适用，详见 §27.3）：禁止扩散到他人 `module` 子树；下游禁止反向 import 上层目录；禁止访问依赖模块内部数据；禁止在 `STATUS: DONE` 后静默改接口签名；禁止循环依赖。

## 22. Performance Expectation

Low 档：RAM < 1.5GB、VRAM < 1GB、Draw Call < 300、三角面 < 300k、同屏实体 ≤ 50、粒子 ≤ 200、场景加载 < 15s、FPS P95 ≥ 30（**最终数字以本任务容量报告实测为准**）。

## 23. Deliverables

- client/resource/resource_manager.gd
- client/resource/scene_streamer.gd
- client/resource/proc_mesh_cache.gd
- client/resource/quality_preset.gd
- client/resource/tests/*
- client/resource/benchmark/*
- config/client/quality.json
- tools/lowspec/profile.gd
- client/resource/docs/INTERFACE.md
- client/resource/docs/PERFORMANCE.md

## 24. Verification Script（本地验收）

**验收脚本**：`scripts/verify/task-036.sh`（手写，执行 `require_godot` 门禁 + Godot 工程校验）

```bash
# 默认 Release；可指定 Debug：BUILD_TYPE=Debug bash scripts/verify/task-036.sh
bash scripts/verify/task-036.sh
```

脚本执行的检查项：

1. 前置任务门禁：`require_tasks_done 034 035`
2. `require_godot`：探测 Godot 4.7.2 可执行（版本匹配），未命中即非零退出
3. 交付物存在性检查（7 项）
4. Godot 工程可加载：`godot --headless --path client --check-only` 退出码 0
5. 测试执行：`godot_run_tests` 过滤 Resource
6. Benchmark 执行：`godot --headless --path client --benchmark-resource --quality low --duration 300`
7. 性能阈值断言：`bench/resource_low.txt` 中 `ram_mb` ≤ `1536`
8. 性能阈值断言：`bench/resource_low.txt` 中 `vram_mb` ≤ `1024`
9. 性能阈值断言：`bench/resource_low.txt` 中 `draw_calls` ≤ `300`
10. 性能阈值断言：`bench/resource_low.txt` 中 `triangles` ≤ `300000`

脚本遵循 `set -euo pipefail`：任一步失败即非零退出，**不存在「警告通过」**。
脚本只报告真实执行结果，禁止兜底伪造 PASS；指标缺失直接判失败，禁止用估算值代替。

## 25. Git Commit

**必须先通过验收脚本（退出码 0），才允许提交。**

```bash
# 1) 把本任务标记为 DONE（脚本会校验 STATUS 流转合法）
bash scripts/task-done.sh TASK-036

# 2) 提交：Conventional Commits，scope 用模块名
git add -A
git commit -F - <<'EOF'
feat(client): Resource / Low Spec System 2.5D (Godot .import + 3D chunk streaming)

- 实现要点：（填写本任务实际落地的内容，禁止复制 Objective）
- 实测数字：（粘贴 scripts/verify/task-036.sh 的真实输出，禁止写「性能良好」）

Refs: TASK-036
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
3. 查依赖：确认 TASK-034, TASK-035 均已 `STATUS: DONE`，否则停止并报告。
4. 查现状：grep 现有代码，确认要改的文件与符号真实存在，禁止凭空假设。
5. 守范围：只改本任务 §14 Files 范围内的文件；发现范围外问题只记录不修改。
6. 做实现：按 §15 Implementation Steps 顺序落地，每步可独立运行。
7. 本地构建：`require_godot` 门禁通过；Godot 工程 `_headless --check-only` 通过。
8. 跑单测：§16 Unit Test 全绿，新增代码必须带测试。
9. 跑集成：§17 Integration Test 全绿。
10. 跑 Benchmark：§18 真实执行，输出机器可读的 `key=value`，禁止估算。
11. 出结果：把实测数字写入 §22 对应的报告文件，不达标如实记录。
12. 跑验收脚本：`bash scripts/verify/task-036.sh` 退出码 0 后，才执行 §25 提交。

> **门禁**：本任务未通过 §20 Acceptance Criteria 与 §24 验收脚本，禁止进入下一个 TASK。

## 27. 接口契约、模块边界与扩展性

本节是**防任务间交付相互干扰 + 保框架可扩展**的统一契约，所有任务适用。

### 27.1 本任务导出的接口（冻结后不可破坏性变更）

见 §7 Public Interface。Godot 类 `ResourceManager` / `SceneStreamer` / `ProcMeshCache` / `QualityPreset` 类名与关键方法签名一旦 `STATUS: DONE` 即视为契约冻结，下游依赖它；破坏性变更须走 `version` + 兼容性评估。

### 27.2 本任务消费的上游接口（来自前置任务，禁止绕过）

- `TASK-034` · `client/runtime`：消费其 `ClientWorld` 玩家位置（驱动 `SceneStreamer`），禁止访问 runtime 内部可变状态。
- `TASK-035` · `client/renderer`：消费其 `QualityPreset` 消费接口与 `LODManager` 距离信号，禁止访问 renderer 内部实现。

### 27.3 模块边界红线（全任务统一）

- 模块 ≠ 进程：本任务代码只落在自身 `module` 子树（`client/resource/`），禁止扩散到其它任务拥有的目录。
- 下游只能通过本任务公开类/资源调用，禁止反向 import 上层目录或访问内部实现（验收脚本静态扫描 `client/ui/`、`client/gameplay/` 不得反向依赖 resource 内部）。
- 本任务只调用依赖模块**声明**的接口，禁止访问其内部数据。
- 接口在 `STATUS: DONE` 之后变更必须走 `version` 字段 + 兼容性评估，禁止静默改签名导致下游编译失败。
- 依赖方向单向（runtime → renderer → resource，且 `client/ui/`、`client/gameplay/` 不得反向依赖），禁止循环依赖。

### 27.4 扩展性约束（可扩展框架兼容性）

- 新增同类能力（新资源类型 / 新分块策略 / 新画质档）必须走**资源/ID 段**机制，禁止在 `match`/`switch` 里硬编码穷举。
- 跨模块扩展点统一用 Godot 信号 / 资源 / 抽象接口，新增实现**不得修改既有任务文件**。
- 协议 / 接口变更必须带 `version` 字段并向下兼容，旧客户端 / 旧模块不得因此断连或编译失败。
- 所有模块遵循统一目录模板（`client/{runtime,network,gameplay,ui,extensions}` + `docs/`），新增模块不得例外。

> 模块归属表（谁拥有哪棵子树）：本任务的 `module` 字段即其独占目录；
> 其它任务的 `module` 字段不得被本任务写入。统一模块模板见 `DEVELOPMENT.md` / 根规范 §6。

## 28. 变更记录

| 日期 | 变更 |
|---|---|
| 2026-08-29 | 方案 A 原地补齐：由 `tools/gen/build_tasks.py` 从结构化数据源重新生成，补齐 State Owner / 验收脚本 / STATUS 门禁 / Git Commit 规范 |
| 2026-09-14 | **2.5D 刷新**：按 `docs/client-spec-2.5d.md` §8 重写——资源管线改为 Godot `.import` + `AtlasTexture`/`TextureAtlas` + 程序化网格缓存；分块流式改为 3D 世界分块（九宫格/半径加载，异步加载、超出延迟 5s 释放防抖）；三档预算增三角面/精灵图集分辨率/地面网格密度；构建门禁改 `require_godot`；STATUS 保持 PENDING |
