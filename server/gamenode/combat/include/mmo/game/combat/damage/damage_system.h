#pragma once

/// TASK-022 · DamageSystem 公开接口（§7 / §15.4）。
///
/// State Owner（§4）：HP 变化的**写入**走 `RoleSystem::ModifyHp`（角色数据的唯一权威是 Role，
/// 见 TASK-016 §4）；DamageSystem 只负责「算多少、按什么顺序算、发什么事件」。
/// 它不自己改 Character::hp —— 那会破坏单写入者约束（§4 / §27.3）。
///
/// 随机（§4 / §21）：全部经 `Prng`（per-Scene 确定性 xorshift128+），
/// 种子 = (SceneId, TickNumber, 序列号)，**禁止全局 rand()**。
///
/// 热路径（§10）：禁止 MySQL / Redis / Kafka / 同步 gRPC / 文件 IO / 堆分配。
/// `alloc_per_damage` 必须为 0 —— 总线事件保持 ≤32B（EventSlot 内联阈值），
/// 完整结算记录走预分配的采样环形缓冲，不进总线。
///
/// 红线（§27.3）：本公开头只 include 依赖模块的公开头（entity / role / scene / core），
/// 禁止 include 任何 `src/`。

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "mmo/core/bus/event_bus.h"
#include "mmo/core/error/result.h"
#include "mmo/game/combat/damage/damage.h"
#include "mmo/game/combat/damage/damage_formula.h"
#include "mmo/game/combat/damage/prng.h"
#include "mmo/game/entity/entity_id.h"
#include "mmo/game/role/attribute.h"
#include "mmo/game/role/character.h"
#include "mmo/game/role/role_system.h"
#include "mmo/game/scene/scene_context.h"

namespace mmo::game::combat {

/// 护盾来源（§15.5）。真实护盾值由 TASK-023 Buff 系统提供，**本任务只定义接口**：
/// 未注入时 `ShieldOf` 一律返回 0，伤害全额进 HP。
class IShieldSource {
public:
    virtual ~IShieldSource() = default;
    /// 目标当前护盾值（无护盾返回 0）。热路径调用，禁止 IO / 分配。
    virtual std::int64_t ShieldOf(EntityId target) const noexcept = 0;
    /// 扣减护盾（amount ≤ ShieldOf(target)）。由 DamageSystem 在吸收阶段调用一次。
    virtual void ConsumeShield(EntityId target, std::int64_t amount) noexcept = 0;
};

/// 战斗统计（§15.7，平衡性分析必需）。
struct DamageStats {
    // ---- 伤害 ----
    std::uint64_t damage_events{0};   // 结算次数（含闪避）
    std::uint64_t hit_count{0};       // 未闪避（产生实际扣血或被完全吸收）
    std::uint64_t crit_count{0};
    std::uint64_t dodge_count{0};
    std::uint64_t blocked_count{0};   // 被护盾完全吸收
    std::uint64_t lethal_count{0};    // 致死次数（与 EntityDied 事件数一致，§17）
    std::uint64_t total_raw{0};
    std::uint64_t total_final{0};     // 总伤害量（Σ final_amount）
    std::uint64_t total_absorbed{0};
    // ---- 治疗 ----
    std::uint64_t heal_events{0};
    std::uint64_t heal_crit_count{0};
    std::uint64_t total_heal{0};      // 有效治疗量（Σ effective）
    std::uint64_t total_overheal{0};
    // ---- 采样日志（§20.6「日志量可测」）----
    std::uint64_t log_records{0};     // 累计写入环形缓冲的采样条数
    std::uint64_t log_dropped{0};     // 缓冲满被丢弃的条数

    /// 观测暴击率（万分比）；无样本返回 0。
    std::int64_t CritRateBp() const noexcept;
    /// 观测闪避率（万分比）；无样本返回 0。
    std::int64_t DodgeRateBp() const noexcept;
    /// 平均每次结算伤害（万分比精度的定点值，避免引入 double）。
    std::int64_t AvgDamageX10000() const noexcept;
};

class DamageSystem {
public:
    /// formula：公式参数（**必填、无默认实参**，须由 `DamageFormula::LoadFromFile` 成功加载，
    ///          禁止默认值静默启动）。
    /// roles / entities：TASK-016 / TASK-011 的公开接口引用。
    /// scene：本 Scene（PRNG 种子的一部分，§4）。bus：事件总线（可空，未绑定则只算不发）。
    /// 注：构造不分配大块内存；采样环形缓冲按 `log_ring_capacity` 一次性预留（§10 零运行时分配）。
    DamageSystem(const DamageFormula& formula, role::RoleSystem& roles, EntityManager& entities,
                 SceneId scene, core::EventBus* bus = nullptr);

    // ---- 装配（宿主在实体进入场景时调用，§21 模块边界由宿主负责）----

    /// 绑定 EntityId ↔ CharacterId（与 TASK-021 `SkillSystem::BindAvatar` 同构）。
    void BindAvatar(EntityId entity, role::CharacterId character) noexcept;
    void UnbindAvatar(EntityId entity) noexcept;
    /// 绑定总线（幂等，重复调用只覆盖指针，不重复订阅 —— TASK-019 教训）。
    void BindEventBus(core::EventBus& bus) noexcept { events_ = &bus; }
    /// 注入护盾来源（TASK-023 接入；可空 = 无护盾）。
    void SetShieldSource(IShieldSource* shields) noexcept { shields_ = shields; }

    // ---- §7 冻结接口 ----

    /// 纯函数结算：**随机量由调用方传入**，不读不写任何系统状态（§15.2 / §9 可跨线程）。
    DamageResult ComputeDamage(const DamageRequest& req, const role::AttributeSet& attacker,
                               const role::AttributeSet& defender,
                               const DamageRolls& rolls) const noexcept;

    /// §7 签名版本：随机取自本系统 per-Scene PRNG（**会推进 PRNG 状态**，非纯函数）。
    /// 返回值里 `remaining_hp` 不填（纯计算拿不到目标当前 HP，见 docs/INTERFACE.md）。
    DamageResult ComputeDamage(const DamageRequest& req, const role::AttributeSet& attacker,
                               const role::AttributeSet& defender) noexcept;

    /// 结算并落状态：改 HP（经 RoleSystem）、消耗护盾、发事件、记统计、采样日志。
    /// `out` 可选：非空则回填完整 `DamageRecord`（离线对账用，不在热路径产生分配）。
    ///
    /// 失败语义（§19）：
    ///   · 目标不存在 / 已死亡 → `NOT_FOUND`（不产生负 HP、不改任何状态）；
    ///   · coefficient 为 NaN / Inf → `INVALID_ARGUMENT`；
    ///   · 目标未绑定角色 → `NOT_FOUND`。
    core::Result<DamageResult> ApplyDamage(const DamageRequest& req, const SceneContext& ctx,
                                           DamageRecord* out = nullptr);

    /// 治疗结算：钳制到 MaxHp，返回实际恢复量与溢出量（§19「治疗溢出」）。
    /// 目标不存在 / 已死亡 → `NOT_FOUND`。
    core::Result<HealResult> ApplyHeal(const HealRequest& req, const SceneContext& ctx);

    // ---- 观测（§6 / §15.7）----

    DamageStats Stats() const noexcept { return stats_; }
    void ResetStats() noexcept { stats_ = DamageStats{}; }

    /// 取走自上次调用以来新写入的采样记录（**不在热路径**）。
    /// 返回的是环形缓冲里尚未消费的区间；调用后读指针前移。
    std::span<const DamageRecord> TakeSamples() noexcept;

    // ---- 随机（§15.3 回放）----

    /// 用 (SceneId, TickNumber, seq) 重新播种（§4）。回放时逐帧调用即可完整复现。
    void Reseed(SceneId scene, std::uint64_t tick, std::uint64_t seq) noexcept;
    Prng::State SaveRng() const noexcept { return prng_.Save(); }
    void RestoreRng(Prng::State st) noexcept { prng_.Restore(st); }

    const DamageFormula& Formula() const noexcept { return formula_; }

private:
    /// EntityIndex → CharacterId 的扁平表（O(1)，与 TASK-021 CooldownTracker 同套路，
    /// 热路径禁止 unordered_map 哈希，§15.3）。配 generation 表防 ABA（TASK-011 世代机制）。
    void EnsureAvatarSlot(std::uint32_t index) noexcept;
    role::CharacterId AvatarOf(EntityId entity) const noexcept;

    /// 采样节流：按 tick_number 计算本 Tick 允许写入的条数（§15.8「每秒采样 N 条」）。
    void TickSamplingBudget(std::uint64_t tick_number) noexcept;

    const DamageFormula& formula_;
    role::RoleSystem& roles_;
    EntityManager& entities_;
    SceneId scene_;
    core::EventBus* events_{nullptr};
    IShieldSource* shields_{nullptr};

    std::vector<role::CharacterId> avatar_char_;  // index → CharacterId（0 = 未绑定）
    std::vector<std::uint32_t> avatar_gen_;       // index → 绑定时的世代号（防 ABA）

    Prng prng_{0};
    DamageStats stats_{};

    // 采样环形缓冲：构造期一次性分配，运行期零分配（§21 alloc_per_damage = 0）。
    std::vector<DamageRecord> ring_;
    std::size_t ring_head_{0};   // 下一个写入位置
    std::size_t ring_count_{0};  // 未消费条数
    std::size_t read_pos_{0};    // TakeSamples 的读指针
    std::uint64_t last_tick_{0};
    std::int64_t sample_budget_{0};  // 本 Tick 剩余可采样条数
};

}  // namespace mmo::game::combat
