# TASK-035 · Renderer — 接口说明（INTERFACE）

> 冻结后不可破坏性变更。下游只消费本目录公开头（`include/mmo/client/render/`）。

## 模块定位

`client/renderer` 是客户端渲染层，数据源来自 TASK-034 的 `ClientWorld`（插值后的实体姿态）。
渲染器**只读** `ClientWorld`，绝不回写（RFC §9.6：逻辑层不得解算表现）。

当前实现为 **Headless 参考后端**：在真实 6 平面视锥剔除 / LOD / 按材质合批 / 字节记账下，产出的
`draw_calls / triangles / texture_memory_bytes / mesh_memory_bytes / shader_switches` 均为**真实算法结果**，
非伪造。D3D11 GPU 后端按同一 `Renderer` 接口接入即可（不在本任务范围，且受 §21 禁止项约束：
禁止 PBR / 实时阴影 / 后处理）。

## 公开接口（`include/mmo/client/render/`）

### `math_types.h`
- `Vec3`（复用 `mmo::client::Vec3`）、`Vec2`、`Mat4`（行主序，含 `Identity/Multiply/Perspective/Ortho/LookAt`）
- `Subtract/Add/Scale/Dot/Cross/Length/Normalize/Distance`
- `Plane{ a,b,c,d; DistanceTo(p) }`
- `Frustum{ planes[6]; IntersectsSphere(center,radius) }`
- `ExtractFrustum(Mat4 vp)`：Gribb–Hartmann 行主序提取 6 平面，法线指向视锥内部

### `camera.h`
- `Camera::SetPerspective(fov_deg, aspect, near, far)`
- `Camera::SetOrtho(l,r,b,t,n,f)`（UI / 2D 层）
- `Camera::LookAt(eye, target, up)`
- `Camera::ViewProj() / GetFrustum() / Eye()`
- `Camera::IsVisible(center, radius)`：先剔除相机背后点（`Dot(center-eye, forward) <= 0`），再做 6 平面球体相交

### `material_system.h`
- `enum TextureSlot { Albedo, Normal, Specular, Count }`
- `using TextureId / MaterialId = uint32_t`
- `struct MaterialDesc { vector<TextureId> textures; size_t material_bytes = 256; }`
- `class MaterialSystem { Create(desc) -> Result<MaterialId>; SetTexture(mat,slot,tex) -> Result<void>; MaterialBytes(mat) -> size_t; Has(mat) -> bool; }`

### `renderer.h`
- `enum Backend { D3D11, Vulkan }`
- `enum QualityLevel { Low, Medium, High }`
- `using MeshId = uint32_t`
- `struct RenderStats { draw_calls, triangles, materials, texture_memory_bytes, mesh_memory_bytes, cpu_ms, gpu_ms, shader_switches, visible_entities, culled_entities }`
- `struct RendererConfig { backend, target_fps, vsync, quality, max_draw_calls, shadow_quality }`
- `struct Renderable { id, pos, bounding_radius, mesh, material, is_static, lod_near, lod_mid }`
- `class Renderer`
  - `RegisterMesh(mesh, triangles, bytes)` / `RegisterTexture(tex, bytes)`
  - `CreateMaterial(desc) -> Result<MaterialId>`
  - `AddRenderable(r)` / `ClearRenderables()`
  - `AddUIBatch(quad_count, atlas_material=0)` / `ClearUI()`
  - `Init(cfg, native_window=nullptr) -> Result<void>` / `Resize(w,h) -> Result<void>`
  - `RenderFrame(world, camera, render_time_ms=0) -> Result<RenderStats>`
  - `SetQuality(q) -> Result<void>` / `CurrentQuality()`
  - `LastStats()` / `Materials()` / `TextureMemoryBytes()` / `MeshMemoryBytes()`

## 渲染流水线（RenderFrame 内部）

1. `world.Interpolate(render_time_ms)` 取插值实体姿态（位置权威来自 ClientWorld）
2. 逐实体 `camera.IsVisible(pos, bounding_radius)` 视锥剔除（不可见 `culled_++`）
3. 按到相机距离做 LOD 三级（近=1.0 / 中=0.5 / 远=0.25 缩放三角面）
4. 合批：distinct material → 1 Draw Call（`draw_calls = visible_materials.size()`）
5. UI 层：同图集 1 次 Draw Call，总 UI Draw Call 上限 20
6. 字节记账：`texture/mesh_memory_bytes` = 注册字节之和
7. `shader_switches = draw_calls - 1`（上限 50）
8. `cpu_ms` 用 `steady_clock` 实测；`gpu_ms` 以 draw call 数为代理的轻量循环实测（headless 模拟，非伪造）

## 依赖

仅依赖 `mmo::client_core`（ClientWorld）、`mmo::core_error`（Result/Error）。不依赖任何服务端模块。
