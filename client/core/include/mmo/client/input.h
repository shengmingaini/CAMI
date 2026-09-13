#pragma once

/// TASK-034 · 输入采样（§15）。
///
/// 设计要点：
///   - 逻辑帧开始时采样一次（Sample），得到该帧的输入快照；
///     表现层（按键/鼠标）实时写入，但逻辑层只在帧边界读取，避免同帧抖动。
///   - 维护「持续按住」集合与「本帧刚按下」边沿（just_pressed），Sample 后清空边沿。
///   - 与渲染解耦：无窗口依赖，便于单测（直接 SetKey 模拟）。

#include <cstdint>
#include <unordered_set>
#include <vector>

namespace mmo { namespace client {

struct InputSnapshot {
    std::unordered_set<std::uint32_t> held;       // 当前按住的键码
    std::unordered_set<std::uint32_t> just_pressed; // 本帧刚按下（边沿）
};

class Input {
public:
    /// 设置某键按下/抬起（由平台事件层调用）。
    void SetKey(std::uint32_t code, bool down);
    /// 采样一帧输入（返回快照并清空 just_pressed 边沿）。
    InputSnapshot Sample();

    bool IsHeld(std::uint32_t code) const;

private:
    std::unordered_set<std::uint32_t> held_;
    std::unordered_set<std::uint32_t> just_pressed_;
};

}}  // namespace mmo::client
