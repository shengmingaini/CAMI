#pragma once

/// TASK-034 · 客户端配置（分辨率 / 帧率 / 网络 / 画质档位）。
///
/// 从 config/client/client.json 读取；文件缺失或字段缺失时回退到安全默认值。
/// 解析使用自带极简 JSON（client/json.h），不引入第三方依赖。

#include "mmo/client/net_client.h"

#include <cstdint>
#include <string>

namespace mmo { namespace client {

enum class QualityTier : std::uint8_t {
    Low = 0,
    Medium = 1,
    High = 2,
    Ultra = 3,
};

inline const char* ToString(QualityTier q) noexcept {
    switch (q) {
        case QualityTier::Low:    return "low";
        case QualityTier::Medium: return "medium";
        case QualityTier::High:   return "high";
        case QualityTier::Ultra:  return "ultra";
    }
    return "medium";
}

enum class WindowMode : std::uint8_t {
    Windowed = 0,
    Borderless = 1,
    Fullscreen = 2,
};

struct ClientConfig {
    // 渲染
    std::uint32_t width = 1280;
    std::uint32_t height = 720;
    WindowMode    window_mode = WindowMode::Windowed;
    std::uint32_t target_fps = 60;   // 渲染目标帧率
    std::uint32_t fixed_fps = 60;    // 逻辑帧率（GameLoop fixed_dt = 1000/fixed_fps ms）

    // 网络（直接交给 NetClient）
    NetConfig net;

    // 画质
    QualityTier quality = QualityTier::Medium;
    bool        vsync = true;

    /// 加载配置（文件缺失 / 解析失败 -> 默认值 + 返回 false）。
    bool Load(const std::string& path);

    /// 应用默认网络参数（当 JSON 未提供网络块时）。便于单测构造。
    void ApplyDefaults();
};

}}  // namespace mmo::client
