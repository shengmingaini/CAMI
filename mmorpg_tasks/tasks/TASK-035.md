---
TASK-ID: TASK-035
NAME: Renderer
PHASE: Phase 8 · 客户端
MODULE: client/renderer
OWNER: Codex / WorkBuddy Agent 实施；@技术总监 二次验收；本地 MinGW MSYS2 g++ + vcpkg 编译验证
STATUS: PENDING
DEPENDENCIES: TASK-034
---

# TASK-035 · Renderer

> 本文件由 `tools/gen/build_tasks.py` 从 `tools/gen/data_*.py` 生成，**禁止手工编辑**。
> 需要改动请修改数据源后重新生成：`python tools/gen/build_tasks.py`

| 字段 | 值 |
|---|---|
| TASK-ID | `TASK-035` |
| NAME | Renderer |
| PHASE | Phase 8 · 客户端 |
| MODULE | `client/renderer` |
| OWNER | Codex / WorkBuddy Agent 实施；@技术总监 二次验收；本地 MinGW MSYS2 g++ + vcpkg 编译验证 |
| STATUS | **PENDING** |
| DEPENDENCIES | `TASK-034` |

---

> **✅ 前置已打通（2026-09-13）：RFC §9.8 触发条件已满足（TASK-030~033 / 037 / 039 / 040 / 041 全部 DONE），Godot 4.7.2 环境已纳入准备流程（下载与运行验证进行中，代理限速下预计约 45 分钟）。本任务解除冻结，可按 Godot 4.7.2 + 2D 规格开工；规格刷新（生成器重写）仍建议但不再作为硬阻塞。**
>
> **历史/背景**：原规格按「自研 C++ 客户端 / 自研渲染器 / 自研资源系统」编写，与 2026-08-29 批准的 Godot 4.7.x 路线（RFC §4.6/§6.1）及 2026-09-10 的 2D 优先约束（RFC §9）冲突，故于 2026-09-10 冻结。现服务端/游戏核心前置已全部完成、Godot 环境就绪，冻结解除。
>
>
> **不变项**（重写时必须保留）：协议契约（TASK-005 的 FlatBuffers schema）、AOI Delta 与快照格式、通过 GDExtension(C++) 下沉协议与热路径的策略、`client/{runtime,network,gameplay,ui,extensions}` 目录分层与单向依赖约束、以及「**逻辑层不得解算表现**」这条纪律（RFC §9.6）。

---

## 1. Objective

实现低复杂度渲染器：Camera / Mesh / Material / Texture / Animation / UI。目标 Low Poly + Simple Lighting + Low Draw Call + Static Batching + LOD，**不要把引擎复杂度做得太高**。

## 2. Dependencies

### 2.1 前置任务

- `TASK-034` · Client Core

### 2.2 门禁规则

验收脚本会先执行 `require_tasks_done 034`：
任一前置任务的 `STATUS` 不是 `DONE`，脚本立即非零退出，**禁止越级实施**。

## 3. Module

`client/renderer`

## 4. State Owner（状态归属）

渲染资源（纹理/网格/材质）由 ResourceManager（TASK-036）拥有并管理生命周期；渲染线程只持有句柄。

> 硬约束：同一实时状态只能有一个权威写入者（见 PROJECT_REQUIREMENTS §10 / §12）。
> 跨模块写入必须走 Command，禁止直接改对方内存。

## 5. Input

TASK-034 ClientWorld（渲染数据源）；低配红线：4核 CPU / 4GB RAM / 1GB VRAM（开发目标，最终以实测为准）

## 6. Output

client/renderer 模块 + 渲染基准 + Draw Call / 显存报告

## 7. Public Interface

```cpp
namespace mmo::client::render {
struct RenderStats { uint32_t draw_calls; uint32_t triangles; uint32_t materials;
                     size_t texture_memory_bytes; size_t mesh_memory_bytes;
                     float cpu_ms; float gpu_ms; uint32_t shader_switches; };
class Renderer { public:
  struct Config { Backend backend{Backend::D3D11}; uint32_t target_fps{60};
                  bool vsync{true}; QualityLevel quality{QualityLevel::Low};
                  uint32_t max_draw_calls{500}; uint32_t shadow_quality{0}; };
  core::Result<void> Init(Config, void* native_window);
  core::Result<void> Resize(uint32_t w, uint32_t h);
  core::Result<RenderStats> RenderFrame(const client::ClientWorld&, const Camera&);
  core::Result<void> SetQuality(QualityLevel);      // Low/Medium/High 热切换
  RenderStats LastStats() const noexcept; };
class Camera { public: void SetPerspective(float fov, float aspect, float near_z, float far_z);
  void LookAt(const Vec3& eye, const Vec3& target); Mat4 ViewProj() const;
  Frustum GetFrustum() const; };                    // 视锥剔除必需
class MaterialSystem { public:
  core::Result<MaterialId> Create(const MaterialDesc&);   // 统一材质，减少切换
  core::Result<void> SetTexture(MaterialId, TextureSlot, TextureId); };
}
```

## 8. Data Model

**渲染预算（Low 档，目标值，最终以实测为准）**

| 指标 | Low 档目标 |
|---|---|
| Draw Calls | < 300 |
| Triangles | < 300k |
| 纹理显存 | < 512MB |
| 网格显存 | < 256MB |
| Shader 切换 | < 50/帧 |
| 光照 | 1 方向光 + 顶点色烘焙，无实时阴影 |

**关键技术**：静态合批（静态物体按材质合批）、视锥剔除、LOD（3 级）、材质统一（减少切换）、实例化（同模型多实例）。
**UI**：独立 UI 层，用正交相机 + 图集合批，UI Draw Call < 20。

## 9. Thread Model

渲染线程独立于逻辑线程；逻辑线程产出不可变渲染快照，渲染线程消费（双缓冲）。禁止渲染线程访问 ClientWorld 可变状态。

## 10. Hot Path

**YES** （每帧执行）

本任务位于 Tick 热路径内，禁止：MySQL / Redis 同步访问 / 同步 gRPC / Kafka 同步访问 / 文件 IO / 网络阻塞 IO / 大规模内存分配（见 PROJECT_REQUIREMENTS §14）。

## 11. External IO

**YES** （加载纹理/网格，异步）

所有外部 IO 必须异步化，禁止出现在 Tick 内。

## 12. Network RPC

**NO**


## 13. Persistence

**NO**


## 14. Files

- client/renderer/include/mmo/client/render/
- client/renderer/src/
- client/renderer/tests/
- client/renderer/benchmark/
- config/client/render.json

## 15. Implementation Steps

1. 选定后端：D3D11（低配目标 GPU 为传统 DX11），预留 Vulkan 抽象层接口但不实现
2. 实现 renderer.h/.cpp：初始化、Resize、RenderFrame、SetQuality
3. 实现 camera.h：透视/正交、视锥体（六个平面）与剔除测试
4. 实现 mesh / material / texture 三个资源句柄与加载（异步加载，主线程不阻塞）
5. 实现静态合批：静态物体按材质分组，合并顶点缓冲（构建期离线 + 运行期按区块）
6. 实现视锥剔除 + LOD 选择（3 级：近/中/远，按距离与屏占比）
7. 实现简单光照：1 个方向光 + 环境光 + 顶点色，**不做 PBR、不做实时阴影**
8. 实现实例化渲染：同模型多实体一次 Draw Call（用于怪物群、植被）
9. 实现 UI 层：正交相机 + 图集合批 + 文本渲染（位图字体，禁用复杂排版引擎）
10. 实现渲染统计：draw_calls / triangles / texture memory / shader switches / cpu_ms / gpu_ms
11. 写测试：视锥剔除正确性（已知位置集合的可见性断言）；LOD 选择；合批后 Draw Call 数下降；UI 批次数；画质热切换

## 16. Unit Test

Camera 视锥构造与剔除；LOD 三级选择阈值；材质创建与纹理绑定；合批分组算法；UI 图集与批次；画质切换；统计字段准确

## 17. Integration Test

渲染一个测试场景（1000 个静态物体 + 200 个动态实体 + 50 个 UI 元素）：Low 档下 Draw Call < 300、三角面 < 300k、纹理显存 < 512MB；画质从 Low 切到 High 不崩溃且显存变化可测；连续渲染 10 分钟无显存泄漏（显存占用曲线平稳）

## 18. Benchmark

bin/render_bench：`draw_calls=` / `triangles=` / `texture_mem_mb=` / `mesh_mem_mb=` / `cpu_ms=` / `gpu_ms=` / `fps_p95=` / `shader_switches=`

## 19. Failure Test

显存不足（注入 2GB 纹理）：按 LRU 降级/拒绝加载，返回明确错误而非崩溃；设备丢失（DXGI DEVICE_REMOVED）：捕获并尝试重建设备；窗口最小化/尺寸为 0：跳过渲染不崩溃；着色器编译失败：记录并使用默认材质，不黑屏崩溃；纹理加载失败：使用占位纹理（洋红）并告警

## 20. Acceptance Criteria

1. Camera / Mesh / Material / Texture / Animation / UI 六项全部实现
2. **Low 档 Draw Call < 300、三角面 < 300k、纹理显存 < 512MB**（benchmark 实测）
3. 静态合批与视锥剔除生效（有对照数据：开启前后 Draw Call 对比）
4. LOD 三级生效
5. 画质 Low/Medium/High 可热切换
6. 连续渲染 10 分钟无显存泄漏
7. 设备丢失可恢复，不崩溃
8. Debug / Release 双构建通过，ctest -R Renderer 全绿

以上每一条都必须在本地真实执行并留证；**任一条不满足即判定本任务未完成**，禁止进入下一个 TASK。

## 21. Forbidden

- 禁止实现 PBR / 实时阴影 / 后处理管线（第一版不做）
- 禁止无 Draw Call 预算地堆特效
- 禁止渲染线程访问 ClientWorld 可变状态
- 禁止主线程同步加载大纹理（必须异步）
- 禁止在无显存回收策略下无限加载资源
- 禁止引入重型第三方引擎（保持轻量）
- 禁止在未实测前宣称支持某具体硬件

> 统一边界红线（全任务适用，详见 §27.3）：禁止扩散到他人 `module` 子树；下游禁止 `#include` 本任务 `src/`；禁止访问依赖模块内部数据；禁止在 `STATUS: DONE` 后静默改接口签名；禁止循环依赖。

## 22. Performance Expectation

Low 档：Draw Call < 300、三角面 < 300k、纹理显存 < 512MB、网格显存 < 256MB、Shader 切换 < 50/帧、CPU 渲染耗时 < 4ms、目标 60 FPS。**最终最低配置与实测 FPS 由 TASK-038 的客户端基准确定，本任务不得宣称兼容某硬件。**

## 23. Deliverables

- client/renderer/include/mmo/client/render/renderer.h
- client/renderer/include/mmo/client/render/camera.h
- client/renderer/include/mmo/client/render/material_system.h
- client/renderer/src/*.cpp
- client/renderer/tests/*
- client/renderer/benchmark/*
- config/client/render.json
- client/renderer/docs/INTERFACE.md
- client/renderer/docs/PERFORMANCE.md

## 24. Verification Script（本地验收）

**验收脚本**：`scripts/verify/task-035.sh`（由生成器产出，禁止手工编辑）

```bash
# 默认 Release；可指定 Debug：BUILD_TYPE=Debug bash scripts/verify/task-035.sh
bash scripts/verify/task-035.sh
```

脚本执行的检查项：

1. 前置任务门禁：`require_tasks_done 034`
2. 交付物存在性检查（6 项）
3. CMake configure + 编译（Debug + Release 双构建）
4. ctest 过滤执行：`-R Renderer`
5. Benchmark 执行：`bin/render_bench --scene test_scene --quality low --duration 600`
6. 性能阈值断言：`bench/render_low.txt` 中 `draw_calls` ≤ `300`
7. 性能阈值断言：`bench/render_low.txt` 中 `texture_mem_mb` ≤ `512`
8. 性能阈值断言：`bench/render_low.txt` 中 `triangles` ≤ `300000`

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
feat(client): Renderer

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

1. 读规范：先读 `PROJECT_REQUIREMENTS.md` 与本任务涉及章节，架构冻结，不得自行推翻。
2. 读任务：完整读完本文件全部章节再动手，禁止只看 Objective 就开始写。
3. 查依赖：确认 TASK-034 均已 `STATUS: DONE`，否则停止并报告。
4. 查现状：grep 现有代码，确认要改的文件与符号真实存在，禁止凭空假设。
5. 守范围：只改本任务 §14 Files 范围内的文件；发现范围外问题只记录不修改。
6. 做实现：按 §15 Implementation Steps 顺序落地，每步可独立编译。
7. 本地编译：MinGW MSYS2 g++ + vcpkg manifest mode（baseline `aae277ac`），Debug 与 Release 都要过。
8. 跑单测：§16 Unit Test 全绿，新增代码必须带测试。
9. 跑集成：§17 Integration Test 全绿。
10. 跑 Benchmark：§18 真实执行，输出机器可读的 `key=value`，禁止估算。
11. 出结果：把实测数字写入 §22 对应的报告文件，不达标如实记录。
12. 跑验收脚本：`bash scripts/verify/task-035.sh` 退出码 0 后，才执行 §25 提交。

> **门禁**：本任务未通过 §20 Acceptance Criteria 与 §24 验收脚本，禁止进入下一个 TASK。

## 27. 接口契约、模块边界与扩展性

本节是**防任务间交付相互干扰 + 保框架可扩展**的统一契约，所有任务适用。

### 27.1 本任务导出的接口（冻结后不可破坏性变更）

见 §7 Public Interface。导出头只放在本任务 `include/` 下，签名一旦 `STATUS: DONE` 即视为契约冻结，下游依赖它；破坏性变更须走 `version` + 兼容性评估。

### 27.2 本任务消费的上游接口（来自前置任务，禁止绕过）

- `TASK-034` · `client/core`：消费其 `include/` 下公开接口（详见该任务 §7 Public Interface），禁止 `#include` 其 `src/`

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
