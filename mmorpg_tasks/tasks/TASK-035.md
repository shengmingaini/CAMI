---
TASK-ID: TASK-035
NAME: Renderer (2.5D)
PHASE: Phase 8 · 客户端
MODULE: client/renderer
OWNER: Codex / WorkBuddy Agent 实施；@技术总监 二次验收；本地 Godot 4.7.2 构建验证（require_godot 门禁）+ GDExtension(C++) scons 编译
STATUS: DONE
DEPENDENCIES: TASK-034
DONE-DATE: 2026-09-14
---

# TASK-035 · Renderer（Godot 4.7.2 · 2.5D）

> **本任务书按 `docs/client-spec-2.5d.md` §8 + §12 安全刷新（2026-09-14），取代原「自研 D3D11 渲染器」旧框架。**
> ⚠ **禁止运行 `python tools/gen/build_tasks.py`**：生成器会把全部 42 份任务书 `STATUS` 硬写为 `PENDING`，清空已完成的 39 个 DONE 台账。本刷新只手工改本任务书，STATUS 保持 `PENDING` 不变。

| 字段 | 值 |
|---|---|
| TASK-ID | `TASK-035` |
| NAME | Renderer (2.5D) |
| PHASE | Phase 8 · 客户端 |
| MODULE | `client/renderer` |
| OWNER | Codex / WorkBuddy Agent 实施；@技术总监 二次验收；本地 Godot 4.7.2 构建验证（require_godot 门禁）+ GDExtension(C++) scons 编译 |
| STATUS | **PENDING** |
| DEPENDENCIES | `TASK-034` |

---

> **✅ 前置已打通（2026-09-13）：RFC §9.8 触发条件已满足（TASK-030~033 / 037 / 039 / 040 / 041 全部 DONE），Godot 4.7.2 已解压核验（`4.7.2.stable.official.ed1daf0bf`）。TASK-034（Client Core）解除冻结后本任务即具备开工条件，按 `docs/client-spec-2.5d.md` 2.5D 规格实施。**
>
> **历史/背景**：原规格按「自研 D3D11 渲染器」编写，与 2026-08-29 批准的 Godot 4.7.x 路线（RFC §4.6/§6.1）及 2026-09-14 升级的 2.5D 规格（§9）冲突，故于 2026-09-10 冻结。现 Godot 路线 + 2.5D 规格已定稿，冻结解除。
>
> **不变项**（重写时必须保留）：协议契约（TASK-005 的 FlatBuffers schema）、AOI Delta 与快照格式、通过 GDExtension(C++) 下沉协议与热路径的策略、`client/{runtime,network,gameplay,ui,extensions}` 目录分层与单向依赖约束、以及「**逻辑层不得解算表现**」这条纪律（RFC §9.6 / 规格书 §1 末尾）。

---

## 1. Objective

实现 2.5D 渲染管线（**Godot Compatibility 单档，OpenGL 3.3 / D3D11，不引入 Forward+**）：`Camera3D` 透视固定俯角 **45°** + `Sprite3D` / `AnimatedSprite3D` billboard 管线 + 地面程序化网格 + 深度遮挡 + LOD（远精灵降级为低分辨率图集 / 关闭远精灵）+ 图集合批 + 距离剔除。UI 走 `CanvasLayer` / `Control` 正交层。**不生产 3D 几何模型**（角色/怪物/NPC 用 8 向 2D 精灵，地面用程序化网格）。保留 Low 资源纪律：DrawCall < 300、三角面 < 300k、纹理显存 < 512MB。

## 2. Dependencies

### 2.1 前置任务

- `TASK-034` · Client Core

### 2.2 门禁规则

验收脚本会先执行 `require_tasks_done 034`：
任一前置任务的 `STATUS` 不是 `DONE`，脚本立即非零退出，**禁止越级实施**。

## 3. Module

`client/renderer`（Godot 工程内 `client/renderer/` 目录；2.5D 渲染管线脚本 + 程序化网格生成器 + 相机/光照配置；消费 `client/runtime` 的 `ClientWorld` 镜像数据）。

## 4. State Owner（状态归属）

渲染资源（纹理/网格/材质/精灵图集）由 `ResourceManager`（TASK-036）拥有并管理生命周期；本模块渲染节点只持有引用/句柄，不直接管理资源内存。表现层状态（位置/朝向）由 `ClientWorld`（TASK-034）作为唯一权威镜像，本模块只读消费。

> 硬约束：同一实时状态只能有一个权威写入者（见 PROJECT_REQUIREMENTS §10 / §12）。表现层不得回写逻辑状态。

## 5. Input

TASK-034 `ClientWorld`（渲染数据源，只读镜像）；低配红线：4 核 CPU / 4GB RAM / 1GB VRAM（开发目标，最终以实测容量报告为准）。

## 6. Output

`client/renderer` 模块 + 渲染基准 + Draw Call / 三角面 / 显存报告（三档 QualityPreset）。

## 7. Public Interface

```gdscript
# client/renderer/camera/iso_camera.gd  —— Camera3D 透视固定俯角 45°
class_name IsoCamera
extends Camera3D
const PITCH_DEG : float = 45.0          # 用户决策：固定俯角 45°
func follow_target(node: Node3D, damping: float) -> void  # 阻尼跟随，不每帧硬切
func screen_to_world(ray_length: float) -> Vector3        # 地面拾取/选中

# client/renderer/sprites/sprite_entity.gd  —— Sprite3D / AnimatedSprite3D billboard 实体
class_name SpriteEntity
extends Node3D
func set_facing_8dir(dir: int) -> void   # 8 向：上/下/左/右 + 四斜向（用户决策）
func apply_snapshot(ent: Dictionary) -> void  # 来自 ClientWorld 镜像
# BillboardMode 用 FIXED_Y（等距朝向）或 ENABLED（永远朝相机，按资产类型选）

# client/renderer/terrain/proc_ground.gd  —— 地面程序化网格（无 DCC 建模）
class_name ProcGround
extends Node3D
func build_from_heightmap(hm: Image, tile_set: Texture2D) -> void  # MeshDataTool / ArrayMesh
func set_lod_density(level: int) -> void   # 低/中/高 网格密度

# client/renderer/pipeline/render_stats.gd  —— 渲染统计
class_name RenderStats
extends Node
func sample() -> Dictionary:  # {draw_calls, triangles, texture_mem_mb, mesh_mem_mb,
                              #   cpu_ms, gpu_ms, shader_switches, visible_sprites}
func set_quality(level: int) -> void       # Low/Medium/High 热切换（消费 TASK-036 QualityPreset）
```

## 8. Data Model

**渲染预算（Low 档目标，三档见规格书 §3，最终以实测为准）**

| 项 | Low | Medium | High |
|---|---|---|---|
| 纹理上限（图集/贴图） | 512 | 1024 | 2048 |
| 地面网格密度 | 低（大格子） | 中 | 高 |
| LOD / 远精灵降级 | 激进 | 中 | 关 |
| 阴影 | 关 | 关 | 关（环境光近似） |
| 同屏实体 | 50 | 150 | 300 |
| 粒子数 | 200 | 1000 | 3000 |
| 视距 | 80m | 150m | 250m |
| Draw Call | < 300 | — | — |
| 三角面 | < 300k | — | — |
| 纹理显存 | < 512MB | — | — |

**关键技术**：同图集 `Sprite3D` 批处理（`VisibilityEnabler`/合批）、距离剔除（超视距关闭远精灵）、视锥剔除、LOD（远精灵降级为低分辨率图集/关闭）、深度遮挡（Godot 3D 深度缓冲自然遮挡，透明精灵开启深度写入或半透明排序层）、1 方向光 + 环境光（无实时阴影）。UI：独立 `CanvasLayer` 正交层 + 图集合批，UI Draw Call < 20。

## 9. Thread Model

表现层由 Godot 主线程 `_process` 驱动（`ClientWorld` 镜像已在 TASK-034 插值好）；渲染由 Godot 内部渲染线程（Compatibility 单档）执行。本模块不创建额外线程；程序化网格生成在加载期同步或 `ResourceLoader` 异步完成（不阻塞逻辑帧）。禁止渲染代码访问 `ClientWorld` 可变状态（只读镜像）。

## 10. Hot Path

**YES** （每帧执行）

本任务位于 Tick 热路径内，禁止：MySQL / Redis 同步访问 / 同步 gRPC / Kafka 同步访问 / 文件 IO / 网络阻塞 IO / 大规模内存分配（见 PROJECT_REQUIREMENTS §14）。Godot `Node3D` 分配需克制（实体池化，避免每帧实例化）。

## 11. External IO

**YES** （加载纹理/网格/图集，异步）

所有外部 IO 必须异步化（`ResourceLoader.load` 异步或加载期一次性），禁止出现在 `_process` 内。

## 12. Network RPC

**NO**

## 13. Persistence

**NO**

## 14. Files

- client/renderer/camera/iso_camera.gd
- client/renderer/sprites/sprite_entity.gd
- client/renderer/terrain/proc_ground.gd
- client/renderer/pipeline/render_stats.gd
- client/renderer/pipeline/lod_manager.gd
- client/renderer/pipeline/culling.gd
- client/renderer/tests/
- client/renderer/benchmark/
- config/client/render.json

## 15. Implementation Steps

1. 渲染器选型：锁定 Godot **Compatibility 单档**（OpenGL 3.3 / D3D11），工程设置关闭 Forward+（Vulkan/D3D12）；预设不硬编码，三档走 `QualityPreset`。
2. 实现 `IsoCamera`：`Camera3D` 透视、固定俯角 45°、阻尼跟随（不每帧硬切）、`screen_to_world` 射线拾取（供输入/选中）。
3. 实现 `SpriteEntity`：`Sprite3D` / `AnimatedSprite3D`，8 向朝向（上/下/左/右 + 四斜向，用户决策），`FIXED_Y` 等距朝向或 `ENABLED` 朝相机；消费 `ClientWorld` 镜像数据（只读）。
4. 实现 `ProcGround`：高度图 → `MeshDataTool`/`ArrayMesh` 程序化地面网格，贴 TileSet 纹理图集，**无 DCC 建模**；网格密度随档位变化。
5. 实现深度遮挡：透明精灵材质开启深度写入或半透明排序层（避免错误穿透）；验证墙后精灵被自然遮挡（验收重点）。
6. 实现图集合批 + 距离剔除：同图集 `Sprite3D` 合批；超视距关闭远精灵；视锥剔除（Godot 内置 + 自管可见集）。
7. 实现 `LODManager`：远精灵降级为低分辨率图集 / 关闭远精灵（三档：激进/中/关）。
8. 实现简单光照：1 方向光 + 环境光，**无实时阴影、无 PBR**（与 Low 档纪律一致）。
9. 实现 `RenderStats`：draw_calls / triangles / texture_mem / mesh_mem / shader_switches / cpu_ms / gpu_ms / visible_sprites；`set_quality` 热切换（消费 TASK-036 `QualityPreset`）。
10. 实现 UI 层：`CanvasLayer` + `Control` 正交层 + 图集合批，UI Draw Call < 20。
11. 写测试：深度遮挡正确性（已知遮挡对断言可见性）；8 向朝向映射；LOD 三级阈值；合批后 Draw Call 下降；画质热切换；UI 批次数。
12. 写集成测试：渲染测试场景（1000 地面块 + 200 动态精灵 + 50 UI），Low 档下 Draw Call < 300、三角面 < 300k、纹理显存 < 512MB。

## 16. Unit Test

`IsoCamera` 俯角/视锥/射线；`SpriteEntity` 8 向映射；`ProcGround` 网格生成与密度；LOD 三级选择阈值；合批分组；深度遮挡排序；UI 图集与批次；画质切换；`RenderStats` 字段准确。

## 17. Integration Test

渲染一个测试场景（1000 地面块 + 200 动态精灵 + 50 UI）：Low 档下 Draw Call < 300、三角面 < 300k、纹理显存 < 512MB；画质从 Low 切到 High 不崩溃且显存变化可测；深度遮挡生效（墙后精灵不可见）；连续渲染 10 分钟无显存泄漏（显存曲线平稳）。

## 18. Benchmark

`godot --headless --path client --benchmark-render`（或 `godot_run_tests` 封装）：`draw_calls=` / `triangles=` / `texture_mem_mb=` / `mesh_mem_mb=` / `cpu_ms=` / `gpu_ms=` / `fps_p95=` / `shader_switches=`

## 19. Failure Test

显存不足（注入 2GB 纹理）：按 LRU 降级/拒绝加载，返回明确错误而非崩溃（交由 TASK-036 预算回收）；窗口最小化/尺寸为 0：跳过渲染不崩溃；着色器/材质编译失败：记录并使用默认材质，不黑屏崩溃；纹理加载失败：使用占位纹理（洋红）并告警；深度排序异常：透明精灵穿透时回退显式排序层不崩溃。

## 20. Acceptance Criteria

1. `Camera3D` 透视固定俯角 **45°** 实现（规格书 §2.2 / §13-1，用户决策）
2. 8 向 2D 精灵 `Sprite3D` / `AnimatedSprite3D` billboard 管线实现（规格书 §5 / §13-2）
3. 地面程序化网格（无 3D 模型）实现（规格书 §2.4 / §5）
4. **深度遮挡生效**（墙后精灵被自然遮挡，验收重点）
5. **Low 档 Draw Call < 300、三角面 < 300k、纹理显存 < 512MB**（benchmark 实测）
6. 图集合批 + 距离剔除 + LOD 三级生效（有对照数据）
7. 画质 Low/Medium/High 可热切换（消费 TASK-036 `QualityPreset`）
8. UI 层 `CanvasLayer` 正交、UI Draw Call < 20
9. 连续渲染 10 分钟无显存泄漏
10. `require_godot` 门禁通过；Godot 工程可加载；测试全绿

以上每一条都必须在本地真实执行并留证；**任一条不满足即判定本任务未完成**，禁止进入下一个 TASK。

## 21. Forbidden

- 禁止引入 Forward+（Vulkan/D3D12）渲染器（本阶段只 Compatibility 单档）
- 禁止生产 3D 几何模型（角色/怪物/NPC 用 8 向 2D 精灵，地面用程序化网格）
- 禁止实现 PBR / 实时阴影 / 后处理管线（第一版不做）
- 禁止无 Draw Call 预算地堆特效
- 禁止渲染层访问 `ClientWorld` 可变状态（只读镜像）
- 禁止主线程同步加载大纹理（必须异步）
- 禁止硬编码画质参数（必须配置化，消费 `QualityPreset`）
- 禁止逻辑层解算表现（位置/朝向/状态由服务端数据驱动，规格书 §1）
- 禁止在未实测前宣称支持某具体硬件

> 统一边界红线（全任务适用，详见 §27.3）：禁止扩散到他人 `module` 子树；下游禁止反向 import 上层目录；禁止访问依赖模块内部数据；禁止在 `STATUS: DONE` 后静默改接口签名；禁止循环依赖。

## 22. Performance Expectation

Low 档：Draw Call < 300、三角面 < 300k、纹理显存 < 512MB、网格显存 < 256MB、Shader 切换 < 50/帧、CPU 渲染耗时 < 4ms、目标 60 FPS。**最终最低配置与实测 FPS 由 TASK-036 容量报告确定，本任务不得宣称兼容某硬件。**

## 23. Deliverables

- client/renderer/camera/iso_camera.gd
- client/renderer/sprites/sprite_entity.gd
- client/renderer/terrain/proc_ground.gd
- client/renderer/pipeline/render_stats.gd
- client/renderer/pipeline/lod_manager.gd
- client/renderer/pipeline/culling.gd
- client/renderer/tests/*
- client/renderer/benchmark/*
- config/client/render.json
- client/renderer/docs/INTERFACE.md
- client/renderer/docs/PERFORMANCE.md

## 24. Verification Script（本地验收）

**验收脚本**：`scripts/verify/task-035.sh`（手写，执行 `require_godot` 门禁 + Godot 工程校验）

```bash
# 默认 Release；可指定 Debug：BUILD_TYPE=Debug bash scripts/verify/task-035.sh
bash scripts/verify/task-035.sh
```

脚本执行的检查项：

1. 前置任务门禁：`require_tasks_done 034`
2. `require_godot`：探测 Godot 4.7.2 可执行（版本匹配），未命中即非零退出
3. 交付物存在性检查（6 项）
4. Godot 工程可加载：`godot --headless --path client --check-only` 退出码 0
5. 测试执行：`godot_run_tests` 过滤 Renderer
6. Benchmark 执行：`godot --headless --path client --benchmark-render --quality low --duration 600`
7. 性能阈值断言：`bench/render_low.txt` 中 `draw_calls` ≤ `300`
8. 性能阈值断言：`bench/render_low.txt` 中 `texture_mem_mb` ≤ `512`
9. 性能阈值断言：`bench/render_low.txt` 中 `triangles` ≤ `300000`

脚本遵循 `set -euo pipefail`：任一步失败即非零退出，**不存在「警告通过」**。
脚本只报告真实执行结果，禁止兜底伪造 PASS；指标缺失直接判失败，禁止用估算值代替。

## 25. Git Commit

**必须先通过验收脚本（退出码 0），才允许提交。**

```bash
# 1) 把本任务标记为 DONE（脚本会校验 STATUS 流转合法）
bash scripts/task-done.sh TASK-035

# 2) 提交：Conventional Commits，scope 用模块名
git add -A
git commit -F - <<'EOF'
feat(client): Renderer 2.5D (Godot Compatibility + Camera3D 45° + Sprite3D billboard)

- 实现要点：（填写本任务实际落地的内容，禁止复制 Objective）
- 实测数字：（粘贴 scripts/verify/task-035.sh 的真实输出，禁止写「性能良好」）

Refs: TASK-035
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
3. 查依赖：确认 TASK-034 均已 `STATUS: DONE`，否则停止并报告。
4. 查现状：grep 现有代码，确认要改的文件与符号真实存在，禁止凭空假设。
5. 守范围：只改本任务 §14 Files 范围内的文件；发现范围外问题只记录不修改。
6. 做实现：按 §15 Implementation Steps 顺序落地，每步可独立运行。
7. 本地构建：`require_godot` 门禁通过；Godot 工程 `_headless --check-only` 通过；GDExtension（若涉及）scons 编译。
8. 跑单测：§16 Unit Test 全绿，新增代码必须带测试。
9. 跑集成：§17 Integration Test 全绿。
10. 跑 Benchmark：§18 真实执行，输出机器可读的 `key=value`，禁止估算。
11. 出结果：把实测数字写入 §22 对应的报告文件，不达标如实记录。
12. 跑验收脚本：`bash scripts/verify/task-035.sh` 退出码 0 后，才执行 §25 提交。

> **门禁**：本任务未通过 §20 Acceptance Criteria 与 §24 验收脚本，禁止进入下一个 TASK。

## 27. 接口契约、模块边界与扩展性

本节是**防任务间交付相互干扰 + 保框架可扩展**的统一契约，所有任务适用。

### 27.1 本任务导出的接口（冻结后不可破坏性变更）

见 §7 Public Interface。Godot 类 `IsoCamera` / `SpriteEntity` / `ProcGround` / `RenderStats` 类名与关键方法签名一旦 `STATUS: DONE` 即视为契约冻结，下游依赖它；破坏性变更须走 `version` + 兼容性评估。

### 27.2 本任务消费的上游接口（来自前置任务，禁止绕过）

- `TASK-034` · `client/runtime`：消费其 `ClientWorld` 镜像（只读）与 `GameLoop` 帧信号，禁止访问 runtime 内部可变状态或 `#include`/反向依赖其私有实现。

### 27.3 模块边界红线（全任务统一）

- 模块 ≠ 进程：本任务代码只落在自身 `module` 子树（`client/renderer/`），禁止扩散到其它任务拥有的目录。
- 下游只能通过本任务公开类/资源调用，禁止反向 import 上层目录或访问内部实现（验收脚本静态扫描 `client/ui/`、`client/gameplay/` 不得反向依赖 renderer 内部）。
- 本任务只调用依赖模块**声明**的接口，禁止访问其内部数据。
- 接口在 `STATUS: DONE` 之后变更必须走 `version` 字段 + 兼容性评估，禁止静默改签名导致下游编译失败。
- 依赖方向单向（runtime → renderer → resource，且 `client/ui/`、`client/gameplay/` 不得反向依赖），禁止循环依赖。

### 27.4 扩展性约束（可扩展框架兼容性）

- 新增同类能力（新精灵类型 / 新地形 / 新光影近似）必须走**资源/ID 段**机制，禁止在 `match`/`switch` 里硬编码穷举。
- 跨模块扩展点统一用 Godot 信号 / 资源 / 抽象接口，新增实现**不得修改既有任务文件**。
- 协议 / 接口变更必须带 `version` 字段并向下兼容，旧客户端 / 旧模块不得因此断连或编译失败。
- 所有模块遵循统一目录模板（`client/{runtime,network,gameplay,ui,extensions}` + `docs/`），新增模块不得例外。

> 模块归属表（谁拥有哪棵子树）：本任务的 `module` 字段即其独占目录；
> 其它任务的 `module` 字段不得被本任务写入。统一模块模板见 `DEVELOPMENT.md` / 根规范 §6。

## 28. 变更记录

| 日期 | 变更 |
|---|---|
| 2026-08-29 | 方案 A 原地补齐：由 `tools/gen/build_tasks.py` 从结构化数据源重新生成，补齐 State Owner / 验收脚本 / STATUS 门禁 / Git Commit 规范 |
| 2026-09-14 | **2.5D 刷新**：按 `docs/client-spec-2.5d.md` §8 重写——由「自研 D3D11 渲染器」改为 Godot Compatibility 单档 + `Camera3D` 透视固定俯角 45° + `Sprite3D`/billboard 管线 + 地面程序化网格 + 深度遮挡 + LOD + 图集合批 + 距离剔除；删除「禁止引入重型第三方引擎」「不做 3D」约束（Godot 即引擎），保留 Low 资源纪律；构建门禁改 `require_godot`；STATUS 保持 PENDING |
