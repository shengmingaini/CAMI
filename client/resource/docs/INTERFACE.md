# TASK-036 · Resource / Low Spec System —— 接口与边界说明

模块目录：`client/resource/`
公开头目录：`client/resource/include/mmo/client/resource/`
链接目标：`mmo_resource`（alias `mmo::resource`）
构建产物：`resource_test`（ctest `Resource.Suite`）、`resource_bench`（`bin/resource_bench.exe`）

> 本模块为 **headless 诚实实现**：无 GPU、无真实磁盘资源，但所有字节占用为**真实记账**
> （`std::vector<uint8_t>` 实际分配 + 确定性尺寸推导），禁止伪造任何数字。

---

## 1. 冻结公开接口（签名不得更改）

### `quality_preset.h`
```cpp
enum class QualityLevel : uint8_t { Low=0, Medium=1, High=2 };

struct QualityPreset {
    QualityLevel level;
    uint32_t texture_max_size;
    uint32_t anisotropic;
    float    lod_bias;
    uint32_t shadow_quality;
    uint32_t max_visible_entities;
    uint32_t max_particles;
    uint32_t net_update_hz;
    float    view_distance;
    uint32_t chunk_radius;
    size_t   texture_budget_bytes;
    size_t   mesh_budget_bytes;
};

core::Result<void>        LoadPresets(std::string_view json_path); // 从 config/client/quality.json 加载三档
core::Result<QualityPreset> GetPreset(QualityLevel);             // 取某档（未加载则用内置缺省）
std::array<QualityPreset,3> DefaultPresets() noexcept;            // 编译期缺省（兜底）
std::string_view QualityLevelName(QualityLevel) noexcept;
core::Result<QualityLevel> QualityLevelFromName(std::string_view);
```

### `resource_manager.h`
```cpp
enum class ResourceType : uint8_t { Texture=0, Mesh=1, Audio=2 };
using ResourceId = uint64_t;
struct ResourceHandle { ResourceId id; bool IsValid() const; };

struct ResourceStats {
    size_t ram_bytes, vram_bytes, texture_bytes, mesh_bytes, audio_bytes;
    uint32_t handle_count, lru_evictions;
    // 扩展（真实派生，不改动上述字段语义）：
    double ram_mb, vram_mb;
    uint32_t texture_count, mesh_count, audio_count;
    uint32_t visible_entities, draw_calls;
    double cache_hit_rate, load_ms_p95;
    uint64_t total_requests, cache_hits;
};

class ResourceManager {
    core::Result<void> Init(const QualityPreset&);
    core::Result<ResourceHandle> LoadAsync(std::string_view uri, ResourceType);
    core::Result<void> Unload(ResourceHandle);
    core::Result<void> SetQuality(QualityLevel);   // 热切换 + 按新预算回收
    ResourceStats Stats() const noexcept;
};
```

### `scene_streamer.h`
```cpp
struct FrameContext { double dt_ms; uint32_t frame_index; };

struct StreamingStats {
    uint32_t loaded_chunks, pending_loads;
    double   last_load_ms;
    uint32_t unload_count;
    uint32_t requested_loads, unloaded_chunks, max_loaded_ever;
};

class SceneStreamer {
    struct Config { float chunk_size{128}; uint32_t load_radius{2};
                    uint32_t unload_delay_seconds{5}; uint32_t max_pending_loads{4}; };
    explicit SceneStreamer(const Config&);
    void Configure(const Config&) noexcept;
    core::Result<void> Update(const Vec3& player_pos, const FrameContext&); // 主线程每帧
    core::Result<void> ForceUnloadAll();
    StreamingStats Stats() const noexcept;
};
```

`Vec3` 复用 `mmo::client::Vec3`（`mmo/client/types.h`），**不重定义**。

---

## 2. 模块边界（红线）

- **仅落在 `client/resource/` 子树内**；不触碰 `client/core`、`client/renderer`、`engine`、`server` 等其它模块目录。
- 下游只能通过 `include/` 下的公开头与接口调用；**禁止 `#include` 本模块 `src/` 或任何内部头**。
- 错误传播使用 `core::Result` / `core::Error` / `core::ErrorCode`，**不使用 `MMO_TRY`**（依赖 GNU 语句表达式，根 `CMAKE_CXX_EXTENSIONS OFF`）。
- 所有测试/benchmark 输出走 `mmo::core::test::Line/LineFmt/Error/ErrorFmt`，**禁止 `std::cout/printf/std::cerr`**。
- 依赖方向单向（resource → client_core / protocol / core_error / core_time）；无反向依赖。

---

## 3. 行为契约

- **异步加载**：`LoadAsync` 立即返回句柄（pending 状态），Worker 线程池（2~4 线程，按 CPU 核数）后台真实分配并记账，主线程调用点（安全点）合并。
- **引用计数**：同 uri 复用并 +1 引用；`Unload` 减引用，归零后成为 LRU 候选，不立即删除。
- **三档独立预算**：纹理受 `texture_budget_bytes`、网格受 `mesh_budget_bytes`、音频受内部常量 `64MB` 约束；互不挤占。
- **LRU 回收**：预算超限时按 LRU（最久未用）回收，先纹理后网格（§15.7）。仅回收 **ref==0** 的条目（被引用的资源不会被强行剥夺，否则句柄悬空）。
- **画质热切换**：`SetQuality` 按新预算回收超预算资源（先纹理后网格）并重建由 preset 派生的 LOD 距离。
- **分块流式**：`SceneStreamer` 按 `chunk_size` 切片；进入半径异步加载，离开半径先进入 `Unloading` 倒计时（迟滞），倒计时内重新进入则取消卸载，防抖；每帧限流最多 `max_pending_loads` 个完成回调。

---

## 4. 统计字段真实来源

| 字段 | 来源 |
|---|---|
| `ram_bytes` | 已提交资源 `data` 缓冲真实分配之和 |
| `vram_bytes` | 纹理+网格按真实维度推导的投影记账（GPU 桩，无实测） |
| `texture/mesh/audio_bytes` | 对应类别真实字节之和 |
| `draw_calls` | `min(已提交网格数, max_visible_entities)`（保证 Low<300） |
| `cache_hit_rate` | 命中 / 总 `LoadAsync` 请求 |
| `load_ms_p95` | 真实加载耗时样本的第 95 百分位（自建 nth_element） |
| `lru_evictions` | 累计 LRU 回收次数 |

---

## 5. 用法示例

```cpp
#include "mmo/client/resource/resource_manager.h"
#include "mmo/client/resource/scene_streamer.h"
#include "mmo/client/resource/quality_preset.h"

auto lp = mmo::client::resource::LoadPresets("config/client/quality.json");
auto preset = mmo::client::resource::GetPreset(mmo::client::resource::QualityLevel::Low).Value();

mmo::client::resource::ResourceManager rm;
rm.Init(preset);
auto h = rm.LoadAsync("tex/hero.png", mmo::client::resource::ResourceType::Texture);
// ... 使用 ...
rm.Unload(h.Value());

mmo::client::resource::SceneStreamer streamer;
streamer.Configure({.chunk_size = 128.0f, .load_radius = preset.chunk_radius,
                     .unload_delay_seconds = 5, .max_pending_loads = 4});
mmo::client::Vec3 player{0,0,0};
mmo::client::resource::FrameContext fc{.dt_ms = 16.0, .frame_index = 0};
streamer.Update(player, fc);
```
