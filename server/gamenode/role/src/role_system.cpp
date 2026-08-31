/// TASK-016 · RoleSystem 实现（§7 / §8 / §15）。
///
/// 热路径（§10）：禁止 MySQL / Redis / gRPC / 文件 IO / 网络阻塞 IO / 大规模分配。
/// Save 只做「序列化意图 + 入队」，真正的落盘在 Persistence 线程（§9 / §11 / §13）。

#include "mmo/game/role/role_system.h"

#include <algorithm>
#include <limits>

#include "mmo/core/error/error.h"
#include "mmo/core/error/error_code.h"
#include "mmo/game/role/role_events.h"

namespace mmo::game::role {

namespace {

core::Error NotFound(const char* what) {
    return core::Error(core::ErrorCode::NOT_FOUND, what, core::domain::kCore);
}
core::Error BadArg(const char* what) {
    return core::Error(core::ErrorCode::INVALID_ARGUMENT, what, core::domain::kCore);
}

/// 饱和加法：避免 int64 溢出后钳制语义反转（§19「禁止 HP 出现负值」）。
std::int64_t SaturatingAdd(std::int64_t base, std::int64_t delta) noexcept {
    constexpr std::int64_t kMax = std::numeric_limits<std::int64_t>::max();
    constexpr std::int64_t kMin = std::numeric_limits<std::int64_t>::min();
    if (delta > 0 && base > kMax - delta) return kMax;
    if (delta < 0 && base < kMin - delta) return kMin;
    return base + delta;
}

}  // namespace

RoleSystem::RoleSystem(IPersistenceAdapter& sink, ExpCurve curve, RoleDefaults def) noexcept
    : sink_(sink), curve_(curve), defaults_(def) {}

// ---------------------------------------------------------------------------
// 加载 / 创建 / 绑定
// ---------------------------------------------------------------------------

core::Result<Character*> RoleSystem::LoadOrCreate(PlayerId pid, CharacterId cid,
                                                  const SceneContext& ctx) {
    if (cid == kInvalidCharacterId) {
        return core::Result<Character*>::Fail(BadArg("invalid character id"));
    }
    // 顺带缓存总线：后续 ModifyHp / AddExp 等无 ctx 的接口靠它发布事件。
    events_ = &ctx.events;

    if (Character* exist = FindOrNull(cid)) return core::Result<Character*>::Ok(exist);

    Character c;
    c.id = cid;
    c.owner = pid;
    c.name = "player_" + std::to_string(pid);
    c.level = 1;
    c.exp = 0;
    c.scene = ctx.id;
    c.version = 1;
    c.flags = kCharFlagDirty;  // 新角色尚未落盘，首次 Save 后清除

    // 初始主属性（三层模型的 base 层；equipment / buff 两层留给 TASK-017 / TASK-023）。
    for (std::size_t i = 0; i < kPrimaryAttrCount; ++i) {
        c.attrs.base[i] = defaults_.starting_primary;
    }
    c.attrs.Recompute();

    // 出生即满血蓝（§15.6：HP/MP 不允许为 0 或超上限）。
    c.hp = c.MaxHp();
    c.mp = c.MaxMp();

    auto [it, inserted] = chars_.emplace(cid, std::move(c));
    if (!inserted) return core::Result<Character*>::Fail(NotFound("character insert failed"));
    by_player_[pid] = cid;

    Character* stored = &it->second;
    Emit(AttributesChanged{cid, stored->version, stored->level});
    return core::Result<Character*>::Ok(stored);
}

core::Result<void> RoleSystem::AttachToScene(CharacterId cid, EntityId avatar, SceneId scene) {
    Character* c = FindOrNull(cid);
    if (c == nullptr) return core::Result<void>::Fail(NotFound("character not found"));
    c->avatar = avatar;
    c->scene = scene;
    ++c->version;
    return core::Result<void>::Ok();
}

// ---------------------------------------------------------------------------
// HP / MP
// ---------------------------------------------------------------------------

core::Result<void> RoleSystem::ModifyHp(CharacterId cid, std::int64_t delta,
                                        core::TraceID /*trace*/) {
    Character* c = FindOrNull(cid);
    if (c == nullptr) return core::Result<void>::Fail(NotFound("character not found"));

    const std::int64_t max_hp = c->MaxHp();
    const std::int64_t old_hp = c->hp;
    // 钳制到 [0, MaxHp]（§21：禁止负值 / 超上限）。
    std::int64_t new_hp = SaturatingAdd(old_hp, delta);
    if (new_hp < 0) new_hp = 0;
    if (new_hp > max_hp) new_hp = max_hp;

    const std::int64_t applied = new_hp - old_hp;   // 实际生效增量（钳制后）
    c->hp = new_hp;
    ++c->version;
    c->flags |= kCharFlagDirty;
    Emit(HpChanged{cid, c->hp, applied});

    // §15.7 死亡状态：HP 归零瞬间发布一次，重复 ModifyHp 不重复发布。
    if (c->hp == 0) {
        if (!IsDead(c->flags)) {
            c->flags |= kCharFlagDead;
            ++stats_.deaths;
            Emit(CharacterDied{cid, c->scene});
        }
    } else if (IsDead(c->flags)) {
        // 复活（治疗回血）：清除死亡标记，后续再次归零可重新发布 CharacterDied。
        c->flags &= ~kCharFlagDead;
    }
    return core::Result<void>::Ok();
}

core::Result<void> RoleSystem::ModifyMp(CharacterId cid, std::int64_t delta,
                                        core::TraceID /*trace*/) {
    Character* c = FindOrNull(cid);
    if (c == nullptr) return core::Result<void>::Fail(NotFound("character not found"));

    const std::int64_t max_mp = c->MaxMp();
    const std::int64_t old_mp = c->mp;
    std::int64_t new_mp = SaturatingAdd(old_mp, delta);
    if (new_mp < 0) new_mp = 0;
    if (new_mp > max_mp) new_mp = max_mp;

    const std::int64_t applied = new_mp - old_mp;
    c->mp = new_mp;
    ++c->version;
    c->flags |= kCharFlagDirty;
    Emit(MpChanged{cid, c->mp, applied});
    return core::Result<void>::Ok();
}

// ---------------------------------------------------------------------------
// 经验 / 升级
// ---------------------------------------------------------------------------

core::Result<std::uint32_t> RoleSystem::AddExp(CharacterId cid, std::uint64_t amount,
                                               core::TraceID /*trace*/) {
    Character* c = FindOrNull(cid);
    if (c == nullptr) return core::Result<std::uint32_t>::Fail(NotFound("character not found"));

    // §19 经验溢出：接近 uint64 上限时钳制并报错，禁止静默回绕。
    constexpr std::uint64_t kMax = std::numeric_limits<std::uint64_t>::max();
    if (amount > kMax - c->exp) {
        c->exp = kMax;
        ++c->version;
        return core::Result<std::uint32_t>::Fail(BadArg("exp overflow"));
    }
    c->exp += amount;

    // §16 跨多级：一次给大量经验应连续升级（每级各发一次 LevelUp）。
    while (c->level < curve_.MaxLevel()) {
        const std::uint64_t need = curve_.ExpToNext(c->level);
        if (need == 0 || c->exp < need) break;
        c->exp -= need;
        ++c->level;
        ApplyLevelUp(*c);
        ++stats_.level_ups;
        ++c->version;
        Emit(LevelUp{cid, c->level, c->version});
        Emit(AttributesChanged{cid, c->version, c->level});
    }
    if (c->level >= curve_.MaxLevel()) c->exp = 0;  // 满级后经验封顶清零

    c->flags |= kCharFlagDirty;
    return core::Result<std::uint32_t>::Ok(c->level);
}

// ---------------------------------------------------------------------------
// 属性 / 存档
// ---------------------------------------------------------------------------

core::Result<void> RoleSystem::RecomputeAttributes(CharacterId cid) {
    Character* c = FindOrNull(cid);
    if (c == nullptr) return core::Result<void>::Fail(NotFound("character not found"));
    c->attrs.Recompute();
    ClampVitals(*c);
    ++c->version;
    Emit(AttributesChanged{cid, c->version, c->level});
    return core::Result<void>::Ok();
}

core::Result<void> RoleSystem::Save(CharacterId cid) {
    Character* c = FindOrNull(cid);
    if (c == nullptr) return core::Result<void>::Fail(NotFound("character not found"));

    // 只入队，不等待落盘（§11 / §21）。
    const auto enqueued = sink_.EnqueueSave(*c);
    if (!enqueued.HasValue()) {
        ++stats_.save_failed;
        // §19：进入重试队列并记录，Tick 不受影响。
        if (std::find(retry_queue_.begin(), retry_queue_.end(), cid) == retry_queue_.end()) {
            retry_queue_.push_back(cid);
        }
        // 透传底层错误码（BUSY = 稍后重试），便于调用方区分「后端不可用」与其它故障。
        return core::Result<void>::Fail(core::Error(enqueued.Err()));
    }
    ++stats_.save_enqueued;
    c->flags &= ~kCharFlagDirty;
    return core::Result<void>::Ok();
}

// ---------------------------------------------------------------------------
// 重试队列 / 容量
// ---------------------------------------------------------------------------

std::size_t RoleSystem::FlushRetries() noexcept {
    std::size_t ok = 0;
    std::vector<CharacterId> still;
    still.reserve(retry_queue_.size());
    for (CharacterId cid : retry_queue_) {
        Character* c = FindOrNull(cid);
        if (c == nullptr) continue;  // 角色已卸载，丢弃
        if (sink_.EnqueueSave(*c).HasValue()) {
            ++stats_.save_enqueued;
            c->flags &= ~kCharFlagDirty;
            ++ok;
        } else {
            still.push_back(cid);
        }
    }
    retry_queue_.swap(still);
    return ok;
}

void RoleSystem::Reserve(std::size_t n) {
    chars_.reserve(n);
    by_player_.reserve(n);
}

// ---------------------------------------------------------------------------
// 内部
// ---------------------------------------------------------------------------

Character* RoleSystem::FindOrNull(CharacterId cid) noexcept {
    auto it = chars_.find(cid);
    return (it == chars_.end()) ? nullptr : &it->second;
}

Character* RoleSystem::Find(CharacterId cid) noexcept { return FindOrNull(cid); }

Character* RoleSystem::FindByPlayer(PlayerId pid) noexcept {
    auto it = by_player_.find(pid);
    if (it == by_player_.end()) return nullptr;
    return FindOrNull(it->second);
}

void RoleSystem::ApplyLevelUp(Character& c) noexcept {
    for (std::size_t i = 0; i < kPrimaryAttrCount; ++i) {
        c.attrs.base[i] += defaults_.primary_per_level;
    }
    c.attrs.Recompute();
    ClampVitals(c);
}

void RoleSystem::ClampVitals(Character& c) noexcept {
    const std::int64_t max_hp = c.MaxHp();
    const std::int64_t max_mp = c.MaxMp();
    if (c.hp < 0) c.hp = 0;
    if (c.hp > max_hp) c.hp = max_hp;
    if (c.mp < 0) c.mp = 0;
    if (c.mp > max_mp) c.mp = max_mp;
}

}  // namespace mmo::game::role
