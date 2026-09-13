#pragma once

/// TASK-036 · 画质预设（配置化，禁用硬编码）。
///
/// 三档（Low / Medium / High）全部从 config/client/quality.json 加载，
/// 任何代码都不得写死画质数字（见 TASK-036 §21 Forbidden）。
///
/// 依赖方向：本头只依赖 core::Result / core::Error（值类型），不反向依赖任何模块。

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "mmo/core/error/result.h"

namespace mmo::client::resource {

/// 三档画质等级（冻结，顺序即优先级：Low < Medium < High）。
enum class QualityLevel : std::uint8_t {
    Low = 0,
    Medium = 1,
    High = 2,
};

/// 单档画质预设。所有字段均来自 quality.json，无缺省魔法值参与运行期逻辑。
struct QualityPreset {
    QualityLevel level = QualityLevel::Low;
    std::uint32_t texture_max_size = 512;   // 纹理边长上限（像素）
    std::uint32_t anisotropic = 1;          // 各向异性过滤等级
    float        lod_bias = 1.0f;            // LOD 偏移（+1 表示更早切换到低模）
    std::uint32_t shadow_quality = 0;       // 0=关 1=低 2=中
    std::uint32_t max_visible_entities = 50; // 同屏实体上限
    std::uint32_t max_particles = 200;       // 粒子数上限
    std::uint32_t net_update_hz = 10;        // 网络更新率（Hz）
    float        view_distance = 80.0f;      // 视距（米）
    std::uint32_t chunk_radius = 1;          // 邻近区块保留圈数
    std::size_t   texture_budget_bytes = 256ULL * 1024 * 1024; // 纹理 VRAM 预算
    std::size_t   mesh_budget_bytes = 128ULL * 1024 * 1024;    // 网格 VRAM 预算
};

/// 编译期内置缺省（仅在 quality.json 缺位时兜底，不用于正常路径）。
std::array<QualityPreset, 3> DefaultPresets() noexcept;

/// 从 JSON 文件加载三档预设（覆盖内置缺省）。
/// 失败返回带路径信息的 core::Error（domain = kData）。
core::Result<void> LoadPresets(std::string_view json_path);

/// 取某档预设。若从未 LoadPresets，返回对应档的编译期缺省。
core::Result<QualityPreset> GetPreset(QualityLevel level);

/// 等级名 <-> 枚举（配置解析与日志用）。
std::string_view QualityLevelName(QualityLevel level) noexcept;
core::Result<QualityLevel> QualityLevelFromName(std::string_view name);

}  // namespace mmo::client::resource
