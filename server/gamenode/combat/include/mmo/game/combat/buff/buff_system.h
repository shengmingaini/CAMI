#pragma once

/// TASK-023 · BuffSystem 公开接口（§7 / §8 / §15 / §21）。
///
/// State Owner（§4）：Buff 运行时状态（实例、层数、到期、护盾余量、控制标记）的 Owner 是
/// Combat System，只在 Scene 线程内写。Buff 不得直接改 Character::hp 或 Final 属性——
/// 属性只经 `RoleSystem::RecomputeAttributes` 重算（写 from_buff 层，§8 / TASK-016 §21）；
/// HP 经 `DamageSystem::ApplyDamage/ApplyHeal`（DOT/HOT，§15）。
///
/// 热路径（§10 / §21）：Tick 禁止后端存储 / 网络 / 文件 IO / 堆分配 / 线程创建；事件 ≤32B 内联；
/// 每个 Buff 实例 ≤64B（benchmark `mem_bytes_per_buff` 硬阈值）。
///
/// 红线（§27.3）：本公开头只 include 依赖模块的公开头，禁止 include 任何 `src/`。

#include <cstdint>
#include <unordered_map>
#include <vector>

#include "mmo/core/bus/event_bus.h"
#include "mmo/core/error/result.h"
#include "mmo/core/time/clock.h"
#include "mmo/game/combat/buff/buff_def.h"
#include "mmo/game/combat/damage/damage_system.h"  // IShieldSource / DamageSystem
#include "mmo/game/entity/entity_id.h"
#include "mmo/game/role/attribute.h"
#include "mmo/game/role/role_system.h"
#include "mmo/game/scene/scene_context.h"

namespace mmo::game::combat::buff {

/// Buff 注册表：持有全部 BuffDef（进程级配置加载，运行期只读）。
class BuffRegistry {
public:
    /// 注册一个 Buff 定义（id 重复 → 失败，禁止静默覆盖）。
    core::Result<void> Add(BuffDef def);

    /// 从进程级 ConfigManager 加载（键路径 `buffs[i].*`，见 buff_config.cpp）。
    /// 同一文件重复加载是幂等的（已加载则跳过，不触发键冲突）。
    core::Result<void> LoadFromConfig(std::string_view file);

    const BuffDef* Find(std::uint32_t id) const noexcept;
    bool Contains(std::uint32_t id) const noexcept { return Find(id) != nullptr; }
    std::size_t Size() const noexcept { return defs_.size(); }

private:
    std::unordered_map<std::uint32_t, BuffDef> defs_;
    std::string loaded_path_;  // 已加载的配置文件路径（幂等去重）
};

/// 单个 Buff 实例（运行期，≤64B）。持有指向注册表 BuffDef 的指针（运行期稳定）。
struct BuffInstance {
    const BuffDef* def{nullptr};        // 8B：指向注册表定义
    role::CharacterId owner{0};          // 8B：归属角色（= target）
    role::CharacterId source{0};        // 8B：原始施加者（DOT/HOT 来源）
    std::uint16_t stacks{1};             // 2B：当前层数
    std::uint16_t _pad{0};               // 2B：对齐
    std::int64_t shield_remaining{0};    // 8B：护盾余量（仅 Shield 类非零）
    core::SteadyTime expire_at{};        // 8B：到期时刻（max()=永久）
    core::SteadyTime next_tick_at{};     // 8B：下次周期结算时刻（max()=无）
    // 合计 52B，8B 对齐后 56B ≤ 64B（benchmark `mem_bytes_per_buff` 硬阈值）
};
static_assert(sizeof(BuffInstance) <= 64,
              "BuffInstance must fit in 64B (mem budget, TASK-023 §22)");

/// Buff 统计（§6 观测性）。
struct BuffStats {
    std::uint64_t applies{0};
    std::uint64_t expires{0};
    std::uint64_t dispels{0};
    std::uint64_t ticks{0};        // 周期结算（DOT/HOT）次数
    std::uint64_t recomputes{0};   // 属性重算次数
};

/// Buff / Debuff 运行时系统（§7）。
///
/// 通过 `combat::IShieldSource` 向 DamageSystem 提供护盾（§15.5 护盾先于 HP）。
class BuffSystem final : public combat::IShieldSource {
public:
    /// defs：Buff 定义注册表（引用，须长于本系统生命周期）。
    /// roles：角色系统（属性重算 / HP 钳制，§4 经接口访问）。
    /// dmg：伤害系统（DOT/HOT 经其结算；可空 = 不结算周期伤害/治疗）。
    /// bus：事件总线（可空，未绑定则只算不发）。
    BuffSystem(const BuffRegistry& defs, role::RoleSystem& roles,
               combat::DamageSystem* dmg = nullptr, core::EventBus* bus = nullptr);

    // ---- 装配（宿主在实体进入场景时调用，§21 模块边界由宿主负责）----
    void BindEventBus(core::EventBus& bus) noexcept { bus_ = &bus; }
    void BindAvatar(EntityId entity, role::CharacterId character) noexcept;
    void SetDamageSystem(combat::DamageSystem* dmg) noexcept { dmg_ = dmg; }

    // ---- combat::IShieldSource（§15.5，DamageSystem 消费）----
    std::int64_t ShieldOf(EntityId target) const noexcept override;
    void ConsumeShield(EntityId target, std::int64_t amount) noexcept override;

    // ---- §7 冻结接口 ----

    /// 施加 Buff：按 stacking 规则处理层数 / 实例，写 from_buff 并重算属性，发布事件。
    /// 角色已有 Buff 数达到 kMaxBuffsPerChar → BUSY（§19 槽位上限）。
    /// 未知 buff_id → NOT_FOUND。
    core::Result<std::uint32_t> Apply(role::CharacterId target, std::uint32_t buff_id,
                                      role::CharacterId source, core::TraceID trace,
                                      core::SteadyTime now);

    /// 移除某角色的全部指定 Buff 实例（Independent 规则下可能有多个）。
    core::Result<void> Remove(role::CharacterId target, std::uint32_t buff_id,
                              RemoveReason reason, core::TraceID trace);

    /// 驱散：仅 dispellable 的 Buff 可被移除，否则 INVALID_ARGUMENT。
    core::Result<void> Dispel(role::CharacterId target, std::uint32_t buff_id,
                              core::TraceID trace);

    /// 每 Tick 驱动：到期清理 + 周期结算（DOT/HOT）+ 控制标记刷新。由 Scene 同线程调用，禁止另起线程。
    void Tick(const SceneContext& ctx);

    /// 角色死亡：清除其全部 Buff（§15.7 死亡解控），发布对应移除事件。
    void OnDeath(role::CharacterId target, core::TraceID trace);

    // ---- 只读访问（非 owning）----
    const std::vector<BuffInstance>* BuffsOf(role::CharacterId target) const noexcept;
    std::size_t ActiveBuffCount(role::CharacterId target) const noexcept;
    /// 角色是否处于某控制状态（位掩码查询，§16 可达）。
    bool HasControlFlag(role::CharacterId target, ControlFlag flag) const noexcept {
        const auto it = control_mask_.find(target);
        return it != control_mask_.end() && (it->second & static_cast<std::uint8_t>(flag)) != 0;
    }
    std::uint8_t ControlMask(role::CharacterId target) const noexcept;

    const BuffStats& Stats() const noexcept { return stats_; }
    void ResetStats() noexcept { stats_ = BuffStats{}; }

    static constexpr std::size_t kMaxBuffsPerChar = 64;  // 单角色 Buff 槽位上限

private:
    /// 由 from_buff 净增量重算角色派生属性（先加后乘合并，§8）。
    void RecomputeFromBuff(role::Character* c) const;
    /// 重算角色控制标记（遍历剩余 Buff 的 control_mask 取并集）。
    void RecomputeControlMask(role::CharacterId target);
    /// 把角色已绑定的实体 id 取出（事件发布用；未绑定返回 0）。
    EntityId EntityOf(role::CharacterId target) const noexcept;

    const BuffRegistry& defs_;
    role::RoleSystem& roles_;
    combat::DamageSystem* dmg_{nullptr};
    core::EventBus* bus_{nullptr};

    std::unordered_map<role::CharacterId, std::vector<BuffInstance>> by_char_;
    std::unordered_map<EntityId, role::CharacterId> avatar_of_;
    std::unordered_map<role::CharacterId, EntityId> char_to_entity_;
    std::unordered_map<role::CharacterId, std::uint8_t> control_mask_;

    mutable BuffStats stats_{};  // 统计计数器：在 const 重算路径中也累加
};

}  // namespace mmo::game::combat::buff
