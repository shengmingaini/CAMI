/// TASK-034 · ClientConfig 实现（JSON 加载 + 默认值回退）。

#include "mmo/client/config.h"
#include "mmo/client/json.h"

#include <fstream>
#include <sstream>
#include <string>

namespace mmo { namespace client {

void ClientConfig::ApplyDefaults() {
    net.addr = "127.0.0.1:9001";
    net.codec = 0;
    net.connect_timeout_ms = 2000;
    net.recv_timeout_ms = 1000;
    net.request_timeout_ms = 5000;
    net.heartbeat_interval_ms = 5000;
    net.reconnect_enabled = true;
    net.max_reconnect_attempts = 5;
    net.reconnect_base_delay_ms = 500;
}

static WindowMode ParseWindowMode(std::string_view s) {
    if (s == "fullscreen") return WindowMode::Fullscreen;
    if (s == "borderless") return WindowMode::Borderless;
    return WindowMode::Windowed;
}

static QualityTier ParseQuality(std::string_view s) {
    if (s == "low")    return QualityTier::Low;
    if (s == "high")   return QualityTier::High;
    if (s == "ultra")  return QualityTier::Ultra;
    return QualityTier::Medium;
}

bool ClientConfig::Load(const std::string& path) {
    ApplyDefaults();
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    std::stringstream ss; ss << in.rdbuf();
    const std::string text = ss.str();
    if (text.empty()) return false;

    auto parsed = JsonParse(text);
    if (!parsed.HasValue()) return false;
    const JsonValue& root = parsed.Value();
    if (!root.IsObject()) return false;

    if (auto* v = root.Find("width"))            width = static_cast<std::uint32_t>(v->AsInt(width));
    if (auto* v = root.Find("height"))           height = static_cast<std::uint32_t>(v->AsInt(height));
    if (auto* v = root.Find("window_mode"))       window_mode = ParseWindowMode(v->AsStr("windowed"));
    if (auto* v = root.Find("target_fps"))        target_fps = static_cast<std::uint32_t>(v->AsInt(target_fps));
    if (auto* v = root.Find("fixed_fps"))         fixed_fps = static_cast<std::uint32_t>(v->AsInt(fixed_fps));
    if (auto* v = root.Find("quality"))           quality = ParseQuality(v->AsStr("medium"));
    if (auto* v = root.Find("vsync"))             vsync = v->AsBool(vsync);

    if (auto* net_block = root.Find("network")) {
        if (net_block->IsObject()) {
            if (auto* v = net_block->Find("gateway_addr"))            net.addr = v->AsStr(net.addr);
            if (auto* v = net_block->Find("codec"))                   net.codec = static_cast<int>(v->AsInt(net.codec));
            if (auto* v = net_block->Find("connect_timeout_ms"))      net.connect_timeout_ms = static_cast<std::uint32_t>(v->AsInt(net.connect_timeout_ms));
            if (auto* v = net_block->Find("recv_timeout_ms"))         net.recv_timeout_ms = static_cast<std::uint32_t>(v->AsInt(net.recv_timeout_ms));
            if (auto* v = net_block->Find("request_timeout_ms"))      net.request_timeout_ms = static_cast<std::uint32_t>(v->AsInt(net.request_timeout_ms));
            if (auto* v = net_block->Find("heartbeat_interval_ms"))   net.heartbeat_interval_ms = static_cast<std::uint32_t>(v->AsInt(net.heartbeat_interval_ms));
            if (auto* v = net_block->Find("reconnect_enabled"))        net.reconnect_enabled = v->AsBool(net.reconnect_enabled);
            if (auto* v = net_block->Find("max_reconnect_attempts"))   net.max_reconnect_attempts = static_cast<int>(v->AsInt(net.max_reconnect_attempts));
            if (auto* v = net_block->Find("reconnect_base_delay_ms")) net.reconnect_base_delay_ms = static_cast<std::uint32_t>(v->AsInt(net.reconnect_base_delay_ms));
        }
    }
    return true;
}

}}  // namespace mmo::client
