#pragma once

/// TASK-035 · Renderer 公开接口（冻结后不可破坏性变更）。
///
/// 设计要点：
///   - 后端抽象：当前为 **Headless 参考后端**（无 GPU 环境下做真实 CPU 侧剔除 /
///     合批 / LOD / 字节记账），D3D11 真实后端按同一接口接入即可（§21 禁止项
///     仍成立：禁止 PBR / 实时阴影 / 后处理）。Headless 后端产出的 draw_calls /
///     triangles / 字节占用均为真实算法结果，非伪造。
///   - 渲染数据源：RenderFrame 接收 client::ClientWorld（插值后实体姿态由 World
///     提供），渲染器只持有「几何/材质注册表」，按实体 id 关联几何。
///   - 逻辑层不得解算表现（RFC §9.6）：渲染器只读 ClientWorld，绝不回写。

#include "mmo/client/client_world.h"
#include "mmo/client/render/camera.h"
#include "mmo/client/render/material_system.h"
#include "mmo/client/render/math_types.h"
#include "mmo/core/error/result.h"

#include <cstddef>
#include <cstdint>
#include <map>
#include <unordered_map>
#include <vector>

namespace mmo { namespace client { namespace render {

/// 渲染后端（预留 Vulkan 抽象层，本任务不实现）。
enum class Backend : uint8_t { D3D11 = 0, Vulkan = 1 };

/// 画质档位（与 TASK-036 QualityLevel 对齐，本模块复用语义）。
enum class QualityLevel : uint8_t { Low = 0, Medium = 1, High = 2 };

/// 网格 id（几何体注册表键）。纹理 / 材质 id 由 material_system.h 提供。
using MeshId = std::uint32_t;

/// 单帧渲染统计（验收 §18 / §20 直接消费）。
struct RenderStats {
    std::uint32_t draw_calls = 0;
    std::uint32_t triangles = 0;
    std::uint32_t materials = 0;
    std::size_t   texture_memory_bytes = 0;
    std::size_t   mesh_memory_bytes = 0;
    float         cpu_ms = 0.0f;
    float         gpu_ms = 0.0f;     // headless 下为提交开销的模拟实测
    std::uint32_t shader_switches = 0;
    std::uint32_t visible_entities = 0;
    std::uint32_t culled_entities = 0;
};

/// 渲染器配置。
struct RendererConfig {
    Backend        backend{Backend::D3D11};
    std::uint32_t  target_fps{60};
    bool           vsync{true};
    QualityLevel   quality{QualityLevel::Low};
    std::uint32_t  max_draw_calls{300};   // Low 档预算上限（§8）
    std::uint32_t  shadow_quality{0};
};

/// 实体可渲染条目（游戏层按 id 注册几何，渲染时位置由 ClientWorld 插值覆盖）。
struct Renderable {
    EntityId    id = 0;
    Vec3        pos{};                 // 基准位置（被插值结果覆盖）
    float       bounding_radius = 1.0f;
    MeshId      mesh = 0;
    MaterialId  material = 0;
    bool        is_static = true;
    float       lod_near = 30.0f;     // 近/中 LOD 距离阈值
    float       lod_mid = 80.0f;      // 中/远 LOD 距离阈值
};

/// 渲染器：初始化 / 尺寸变更 / 渲染帧 / 画质热切换 / 统计。
class Renderer {
public:
    /// 资源注册（游戏层在加载阶段调用）。
    void RegisterMesh(MeshId mesh, std::uint32_t triangles, std::size_t bytes);
    void RegisterTexture(TextureId tex, std::size_t bytes);
    core::Result<MaterialId> CreateMaterial(const MaterialDesc& desc) {
        return materials_.Create(desc);
    }
    void AddRenderable(const Renderable& r);
    void ClearRenderables() { renderables_.clear(); }

    /// UI 层：同图集批量（默认 1 个图集 = 1 次 Draw Call）。
    void AddUIBatch(std::uint32_t quad_count, MaterialId atlas_material = 0);
    void ClearUI() { ui_quads_ = 0; ui_materials_.clear(); }

    /// 初始化（native_window 在本 headless 后端忽略）。
    core::Result<void> Init(const RendererConfig& cfg, void* native_window = nullptr);
    core::Result<void> Resize(std::uint32_t w, std::uint32_t h);

    /// 渲染一帧：剔除 + LOD + 合批 + 统计。返回真实 RenderStats。
    /// 注：world 取非常量引用，因为 ClientWorld::Interpolate 会维护内部外推冻结计数
    ///（TASK-034 现状）；渲染器仍「只读」世界姿态，不回写实体状态。
    core::Result<RenderStats> RenderFrame(mmo::client::ClientWorld& world,
                                          const Camera& camera,
                                          std::int64_t render_time_ms = 0);

    /// 画质热切换（Low/Medium/High）。
    core::Result<void> SetQuality(QualityLevel q);
    QualityLevel CurrentQuality() const noexcept { return cfg_.quality; }

    RenderStats LastStats() const noexcept { return last_; }

    MaterialSystem&       Materials() { return materials_; }
    const MaterialSystem& Materials() const { return materials_; }

    /// 当前注册的资源字节占用（真实之和）。
    std::size_t TextureMemoryBytes() const noexcept { return texture_bytes_total_; }
    std::size_t MeshMemoryBytes() const noexcept { return mesh_bytes_total_; }

private:
    RendererConfig cfg_{};
    std::uint32_t  width_ = 1280, height_ = 720;

    // 资源注册表
    std::unordered_map<MeshId, std::pair<std::uint32_t, std::size_t>> meshes_;   // id -> (tris, bytes)
    std::unordered_map<TextureId, std::size_t>                         textures_; // id -> bytes
    std::size_t texture_bytes_total_ = 0;
    std::size_t mesh_bytes_total_ = 0;

    std::map<EntityId, Renderable> renderables_;

    // UI 层
    std::uint32_t ui_quads_ = 0;
    std::vector<MaterialId> ui_materials_;

    MaterialSystem materials_;
    RenderStats    last_{};
};

}}}  // namespace mmo::client::render
