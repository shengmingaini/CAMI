#pragma once

/// TASK-035 · 统一材质系统（§7 公开接口）。
///
/// 材质统一（减少切换）：一个 Material 聚合若干纹理槽位，渲染合批按材质分组，
/// 因此同材质多实例 = 1 次 Draw Call。材质字节占用真实记账。

#include "mmo/core/error/result.h"

#include <cstdint>
#include <unordered_map>
#include <vector>

namespace mmo { namespace client { namespace render {

/// 纹理槽位。
enum class TextureSlot : std::uint8_t { Albedo = 0, Normal = 1, Specular = 2, Count = 3 };

using TextureId = std::uint32_t;
using MaterialId = std::uint32_t;

/// 统一材质描述。
struct MaterialDesc {
    std::vector<TextureId> textures;     // 各槽位纹理 id（可为空）
    std::size_t            material_bytes = 256;  // 材质 CPU 状态字节（真实记账）
};

/// 材质系统：创建 / 纹理绑定。
class MaterialSystem {
public:
    core::Result<MaterialId> Create(const MaterialDesc& desc);
    core::Result<void>       SetTexture(MaterialId mat, TextureSlot slot, TextureId tex);

    /// 内部：材质字节占用（含描述中的 material_bytes）。
    std::size_t MaterialBytes(MaterialId mat) const;
    /// 内部：该材质是否有效。
    bool Has(MaterialId mat) const noexcept { return mats_.count(mat) != 0; }

private:
    std::unordered_map<MaterialId, MaterialDesc> mats_;
    MaterialId next_id_ = 1;
};

}}}  // namespace mmo::client::render
