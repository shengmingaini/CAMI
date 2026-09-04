#pragma once

/// TASK-022 · per-Scene 确定性 PRNG（§4 / §15.3 / §19）。
///
/// 红线（§21 / §4）：**禁止使用全局 rand() / std::mt19937 之类的进程级随机源**——
/// 伤害结算的随机必须可复现，否则无法回放与对账。
///
/// 算法：xorshift128+（两个 uint64 状态，周期 2^128-1，通过 BigCrush，三指令/次）。
/// 选它而非 PCG / MT19937 的理由：
///   · 状态仅 16B，可整块 Save/Restore（回放必需，§15.3「支持种子设置与状态保存」）；
///   · 无乘法的 64 位实现，ComputeDamage 热路径预算只有 50ns，容不下 MT19937 的 2.5KB 状态。
///
/// 种子构造（§4 硬性要求）：`Seed(SceneId, TickNumber, 序列号)`——
/// 三元组经 SplitMix64 充分混合后写入状态，保证：
///   · 同 Scene 同 Tick 同序号 ⇒ 同序列（回放可复现）；
///   · 不同 Scene / 不同 Tick ⇒ 序列不相关（避免全服同帧同暴击的视觉诡异）。

#include <cstdint>

#include "mmo/game/entity/entity_id.h"  // SceneId（TASK-011 的 mmo::game::SceneId）

namespace mmo::game::combat {

class Prng {
public:
    /// PRNG 全状态（16B）。POD，可直接落盘/网络传输用于回放（§15.3）。
    struct State {
        std::uint64_t s0{0};
        std::uint64_t s1{0};
    };

    /// 只允许显式播种构造：**禁止无参默认构造**，杜绝「忘了播种就用随机数」的静默事故
    /// （TASK-016 教训：ExpCurve()=delete 就是为同一类事故设的闸门）。
    Prng() = delete;
    explicit Prng(std::uint64_t seed) noexcept { SeedRaw(seed); }

    /// 由 (SceneId, TickNumber, 序列号) 播种（§4）。
    void Seed(SceneId scene, std::uint64_t tick, std::uint64_t seq) noexcept {
        // SplitMix64 混合三元组：先把三个输入各自雪崩，再异或，避免低位相关。
        SeedRaw(Mix(scene) ^ (Mix(tick) << 1) ^ (Mix(seq) << 2));
    }

    /// 原始种子播种（测试用）。
    void SeedRaw(std::uint64_t seed) noexcept {
        const std::uint64_t a = Mix(seed);
        const std::uint64_t b = Mix(seed ^ 0x9E37'79B9'7F4A'7C15ull);
        s_ = State{a == 0 ? 1u : a, b == 0 ? 2u : b};  // 全零状态是 xorshift 的不动点，必须避开
    }

    /// 下一个 64 位随机数（xorshift128+）。
    std::uint64_t Next() noexcept {
        std::uint64_t x = s_.s0;
        const std::uint64_t y = s_.s1;
        s_.s0 = y;
        x ^= x << 23;
        x ^= x >> 17;
        x ^= y ^ (y >> 26);
        s_.s1 = x;
        return x + y;
    }

    /// 下一个 [0, scale) 区间的随机数——用于万分比/百分比较验（scale = rate_scale，默认 10000）。
    ///
    /// 定点缩放而非取模：xorshift128+ 的**低位**质量弱于高位，直接 `% scale` 会引入可观测的
    /// 分布偏斜。这里取高 32 位乘以 scale 再右移 32（等价于 v * scale / 2^32），
    /// 结果严格落在 [0, scale)。溢出安全性：v < 2^32 且 scale < 2^17 ⇒ 乘积 < 2^49，int64 足够。
    std::uint32_t NextScaled(std::uint32_t scale) noexcept {
        if (scale == 0) return 0;
        const std::uint32_t v = static_cast<std::uint32_t>(Next() >> 32);
        return static_cast<std::uint32_t>((static_cast<std::uint64_t>(v) * scale) >> 32);
    }

    /// 状态保存 / 恢复（§15.3 回放）。Restore 后后续序列与保存点**完全一致**（§19 单测）。
    State Save() const noexcept { return s_; }
    void Restore(State st) noexcept { s_ = st; }

private:
    /// SplitMix64 的雪崩尾函数：把一个整数充分扩散到 64 位。
    static std::uint64_t Mix(std::uint64_t z) noexcept {
        z += 0x9E37'79B9'7F4A'7C15ull;
        z = (z ^ (z >> 30)) * 0xBF58'476D'1CE4'E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D0'49BB'1331'11EBull;
        return z ^ (z >> 31);
    }

    State s_{};
};

}  // namespace mmo::game::combat
