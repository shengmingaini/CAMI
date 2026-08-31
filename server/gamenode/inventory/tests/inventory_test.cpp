// server/gamenode/inventory/tests/inventory_test.cpp — TASK-017 §16 / §17 / §19
//
// 覆盖：Add/Remove 数量与边界（0、超量、堆叠上限）；Equip/Unequip 槽位与等级校验；
// guid 全局唯一（100 万次无重复）；耐久衰减与归零；事件完整性与字段；容量上限（满包 BUSY
// 物品不丢失）；1000 玩家 × 多操作集成（物品守恒、guid 无重复、事件数 == 操作数）。
//
// 输出统一走 mmo::core::test（test_print.h），禁止裸 cout/printf。
//
// 注：InventorySystem::Add 公开签名返回 core::Result<uint32_t>（实际加入量，冻结契约 §7），
// 物品 guid 经 ItemAdded 事件留痕；测试中通过订阅 ItemAdded 捕获 guid。

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <random>
#include <unordered_set>
#include <vector>

#include "test_print.h"

#include "mmo/core/bus/event_bus.h"
#include "mmo/core/memory/arena.h"
#include "mmo/core/sched/scheduler.h"
#include "mmo/core/time/clock.h"
#include "mmo/game/entity/entity_manager.h"
#include "mmo/game/role/attribute.h"
#include "mmo/game/role/character.h"
#include "mmo/game/role/exp_curve.h"
#include "mmo/game/role/persistence_adapter.h"
#include "mmo/game/role/role_system.h"
#include "mmo/game/scene/scene.h"
#include "mmo/game/scene/scene_context.h"
#include "mmo/game/inventory/commands.h"
#include "mmo/game/inventory/inventory.h"
#include "mmo/game/inventory/inventory_system.h"
#include "mmo/game/inventory/item.h"

namespace {

namespace core = mmo::core;
using namespace mmo::game;
using namespace mmo::game::inventory;
using namespace mmo::game::role;

using mmo::core::test::ErrorFmt;
using mmo::core::test::LineFmt;

int g_fail = 0;
#define CHECK(cond)                                                        \
    do {                                                                   \
        if (!(cond)) {                                                     \
            ErrorFmt("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);       \
            ++g_fail;                                                      \
        }                                                                  \
    } while (0)

// ItemAdded 事件留痕的 guid（测试内捕获用）
ItemGuid g_last_added_guid = 0;

struct EventCounts {
    std::size_t added = 0, removed = 0, equipped = 0, unequipped = 0, durability = 0;
    void Reset() { added = removed = equipped = unequipped = durability = 0; }
    std::size_t Total() const { return added + removed + equipped + unequipped + durability; }
};

// 全局事件计数（订阅 lambda 直接引用，避免捕获）
EventCounts g_ev;

struct AddRes {
    std::uint32_t added{0};
    ItemGuid guid{0};
};

struct Harness {
    core::EventBus bus;
    core::Scheduler scheduler;
    core::Arena arena{256 * 1024};
    EntityManager mgr{&bus};
    SceneContext ctx;
    InMemoryPersistenceAdapter sink;
    ExpCurve curve;
    RoleSystem role;
    ItemDefStore store;
    InventorySystem inv;

    static ExpCurve LoadCurve() {
        auto r = ExpCurve::LoadFromFile("config/gameplay/exp_curve.json");
        if (!r.HasValue()) {
            ErrorFmt("FAIL: cannot load exp_curve.json\n");
            std::exit(1);
        }
        return std::move(r).Value();
    }

    Harness()
        : ctx(7, SceneType::World, 1, core::MonotonicClock::Point(), 0, mgr, bus, scheduler, arena),
          curve(LoadCurve()),
          role(sink, curve),
          inv(store, role) {
        auto lr = store.LoadDir("config/gameplay/items");
        if (!lr.HasValue()) {
            ErrorFmt("FAIL: load items: %s\n", lr.Err().Message().data());
            std::exit(1);
        }
        role.BindEventBus(bus);
        inv.BindEventBus(bus);
    }

    role::Character* EnsureChar(PlayerId p) {
        auto c = role.LoadOrCreate(p, 1000u + p, ctx);
        return c.HasValue() ? c.Value() : nullptr;
    }

    // Add 并经事件捕获 guid（用于需要 guid 的测试）；返回实际加入量与 guid。
    AddRes Add(PlayerId p, ItemId d, std::uint32_t c) {
        auto r = inv.Add(p, d, c, 0);
        CHECK(r.HasValue());
        (void)bus.Drain();  // 派发以捕获 guid（同时计入事件数）
        return AddRes{r.Value(), g_last_added_guid};
    }
};

void DrainAll(core::EventBus& bus) {
    for (;;) {
        auto rem = bus.Drain();
        if (!rem.HasValue()) break;
        if (rem.Value() == 0) break;
    }
}

}  // namespace

int main() {
    Harness h;
    g_ev.Reset();

    (void)h.bus.Subscribe<ItemAdded>([](const ItemAdded& e) { ++g_ev.added; g_last_added_guid = e.guid; });
    (void)h.bus.Subscribe<ItemRemoved>([](const ItemRemoved&) { ++g_ev.removed; });
    (void)h.bus.Subscribe<ItemEquipped>([](const ItemEquipped&) { ++g_ev.equipped; });
    (void)h.bus.Subscribe<ItemUnequipped>([](const ItemUnequipped&) { ++g_ev.unequipped; });
    (void)h.bus.Subscribe<DurabilityChanged>([](const DurabilityChanged&) { ++g_ev.durability; });

    // ---- 1. 堆叠 / 边界 ----
    {
        g_ev.Reset();
        CHECK(h.EnsureChar(1) != nullptr);
        auto a1 = h.Add(1, 1, 50);  // 药水 id=1 max_stack=99
        CHECK(a1.added == 50);
        CHECK(h.inv.View(1)->TotalCount() == 50);
        CHECK(h.inv.UsedSlots(1) == 1);

        auto a2 = h.Add(1, 1, 60);  // 50+49=99 满栈，余 11 新栈
        CHECK(a2.added == 60);
        CHECK(h.inv.View(1)->TotalCount() == 110);
        CHECK(h.inv.UsedSlots(1) == 2);

        const ItemGuid g1 = a1.guid;
        auto r1 = h.inv.Remove(1, g1, 10, 0);  // 栈内扣减
        CHECK(r1.HasValue() && r1.Value() == 10);
        CHECK(h.inv.View(1)->TotalCount() == 100);
        CHECK(h.inv.UsedSlots(1) == 2);

        auto r2 = h.inv.Remove(1, g1, 200, 0);  // 超量拒绝，不扣减
        CHECK(!r2.HasValue());
        CHECK(h.inv.View(1)->TotalCount() == 100);

        auto r3 = h.inv.Remove(1, g1, 89, 0);  // 清空该栈，回收槽
        CHECK(r3.HasValue() && r3.Value() == 89);
        CHECK(h.inv.View(1)->TotalCount() == 11);
        CHECK(h.inv.UsedSlots(1) == 1);

        auto r4 = h.inv.Remove(1, 999999u, 1, 0);  // guid 不存在
        CHECK(!r4.HasValue());

        DrainAll(h.bus);
        CHECK(g_ev.added == 2);
        CHECK(g_ev.removed == 2);  // r1 + r3 成功（r2/r4 失败不发布）
    }

    // ---- 2. 背包满 BUSY，物品不丢失 ----
    {
        g_ev.Reset();
        CHECK(h.EnsureChar(2) != nullptr);
        for (int i = 0; i < 100; ++i) {
            auto r = h.inv.Add(2, 10, 1, 0);  // 铁剑 id=10 max_stack=1
            CHECK(r.HasValue());
        }
        CHECK(h.inv.UsedSlots(2) == 100);
        CHECK(h.inv.View(2)->TotalCount() == 100);

        auto over = h.inv.Add(2, 10, 1, 0);  // 第 101 个 → BUSY
        CHECK(!over.HasValue());
        CHECK(over.Err().Code() == core::ErrorCode::BUSY);
        CHECK(h.inv.View(2)->TotalCount() == 100);  // 总数不变（不产生、不丢失）
        DrainAll(h.bus);
    }

    // ---- 3. 装备槽位不匹配 / 等级不足 ----
    {
        g_ev.Reset();
        CHECK(h.EnsureChar(3) != nullptr);
        auto sword = h.Add(3, 10, 1);  // 铁剑：仅 MainHand 位
        auto badslot = h.inv.Equip(3, sword.guid, EquipSlot::OffHand, 0);
        CHECK(!badslot.HasValue());  // 槽位不匹配

        CHECK(h.EnsureChar(4) != nullptr);
        auto ring = h.Add(4, 17, 1);  // 戒指·力量：required_level=10
        auto lvl = h.inv.Equip(4, ring.guid, EquipSlot::Ring1, 0);
        CHECK(!lvl.HasValue());  // 等级不足
        DrainAll(h.bus);
    }

    // ---- 4. 装备属性联动（from_equipment 层） ----
    {
        g_ev.Reset();
        role::Character* c = h.EnsureChar(5);
        CHECK(c != nullptr);
        auto sword = h.Add(5, 10, 1);  // 铁剑 attr_bonus[Attack]=20
        const std::int64_t before = c->attrs.Total(role::AttrType::Attack);
        auto eq = h.inv.Equip(5, sword.guid, EquipSlot::MainHand, 0);
        CHECK(eq.HasValue());
        CHECK(c->attrs.Total(role::AttrType::Attack) == before + 20);  // 全额耐久
        CHECK(h.inv.View(5)->Equipped(EquipSlot::MainHand) == sword.guid);

        auto un = h.inv.Unequip(5, EquipSlot::MainHand, 0);
        CHECK(un.HasValue());
        CHECK(c->attrs.Total(role::AttrType::Attack) == before);  // 卸下后回到 base
        DrainAll(h.bus);
        CHECK(g_ev.equipped == 1 && g_ev.unequipped == 1);
    }

    // ---- 5. 耐久衰减与归零（属性减半 / 失效但物品保留） ----
    {
        g_ev.Reset();
        role::Character* c = h.EnsureChar(6);
        CHECK(c != nullptr);
        const std::int64_t before = c->attrs.Total(role::AttrType::Attack);  // 装备前基线
        auto sword = h.Add(6, 10, 1);  // max_durability=100
        CHECK(h.inv.Equip(6, sword.guid, EquipSlot::MainHand, 0).HasValue());
        CHECK(c->attrs.Total(role::AttrType::Attack) == before + 20);  // 全额耐久

        CHECK(h.inv.DamageDurability(6, EquipSlot::MainHand, 30, 0).HasValue());  // 70 (>50% 满)
        CHECK(c->attrs.Total(role::AttrType::Attack) == before + 20);
        CHECK(h.inv.DamageDurability(6, EquipSlot::MainHand, 30, 0).HasValue());  // 40 (<50% 减半)
        CHECK(c->attrs.Total(role::AttrType::Attack) == before + 10);
        CHECK(h.inv.DamageDurability(6, EquipSlot::MainHand, 40, 0).HasValue());  // 0 失效
        CHECK(c->attrs.Total(role::AttrType::Attack) == before);
        CHECK(h.inv.View(6)->Equipped(EquipSlot::MainHand) == sword.guid);  // 物品保留
        CHECK(h.inv.View(6)->EquippedStack(EquipSlot::MainHand).durability == 0);
        DrainAll(h.bus);
        CHECK(g_ev.durability == 3);
    }

    // ---- 6. 事件完整性（单玩家全流程） ----
    {
        g_ev.Reset();
        CHECK(h.EnsureChar(7) != nullptr);
        auto potion = h.Add(7, 1, 10);
        auto sword = h.Add(7, 10, 1);
        CHECK(h.inv.Equip(7, sword.guid, EquipSlot::MainHand, 0).HasValue());
        CHECK(h.inv.Remove(7, potion.guid, 5, 0).HasValue());
        CHECK(h.inv.Unequip(7, EquipSlot::MainHand, 0).HasValue());
        DrainAll(h.bus);
        CHECK(g_ev.added == 2);
        CHECK(g_ev.removed == 1);
        CHECK(g_ev.equipped == 1);
        CHECK(g_ev.unequipped == 1);
    }

    // ---- 7. 集成：1000 玩家 × 多操作（物品守恒 / guid 无重复 / 事件数 == 操作数） ----
    {
        g_ev.Reset();
        std::int64_t global_net = 0;
        std::int64_t global_alive = 0;
        std::vector<ItemGuid> all_guids;
        all_guids.reserve(3000);
        std::size_t ops = 0;

        for (PlayerId p = 1001; p <= 2000; ++p) {  // 与前面各节玩家隔离，避免背包已占满
            CHECK(h.EnsureChar(p) != nullptr);
            auto potion = h.Add(p, 1, 20);  ++ops;  global_net += potion.added;
            auto sword  = h.Add(p, 10, 1);  ++ops;  global_net += sword.added;
            auto shield = h.Add(p, 11, 1);  ++ops;  global_net += shield.added;

            CHECK(h.inv.Equip(p, sword.guid, EquipSlot::MainHand, 0).HasValue());  ++ops;
            CHECK(h.inv.Equip(p, shield.guid, EquipSlot::OffHand, 0).HasValue());  ++ops;
            CHECK(h.inv.Unequip(p, EquipSlot::MainHand, 0).HasValue());  ++ops;
            CHECK(h.inv.Unequip(p, EquipSlot::OffHand, 0).HasValue());  ++ops;
            CHECK(h.inv.Equip(p, sword.guid, EquipSlot::MainHand, 0).HasValue());  ++ops;
            CHECK(h.inv.DamageDurability(p, EquipSlot::MainHand, 50, 0).HasValue());  ++ops;

            auto rem = h.inv.Remove(p, potion.guid, 5, 0);  ++ops;
            CHECK(rem.HasValue());
            global_net -= static_cast<std::int64_t>(rem.Value());

            const Inventory* inv = h.inv.View(p);
            global_alive += static_cast<std::int64_t>(inv->TotalCount());
            for (std::size_t s = 0; s < kEquipSlotCount; ++s) {
                const ItemStack& eq = inv->EquippedStack(static_cast<EquipSlot>(s));
                if (eq.guid != 0) { global_alive += eq.count; all_guids.push_back(eq.guid); }
            }
            for (SlotIndex i = 0; i < kMaxInventorySlots; ++i) {
                const ItemStack* st = inv->At(i);
                if (st != nullptr && st->guid != 0) all_guids.push_back(st->guid);
            }
        }

        DrainAll(h.bus);
        CHECK(global_alive == global_net);  // 物品守恒
        CHECK(g_ev.Total() == ops);         // 事件数 == 操作数

        std::sort(all_guids.begin(), all_guids.end());
        bool dup = false;
        for (std::size_t i = 1; i < all_guids.size(); ++i) {
            if (all_guids[i] == all_guids[i - 1]) { dup = true; break; }
        }
        CHECK(!dup);  // 无物品复制
        LineFmt("integration: players=1000 ops=%zu alive=%lld guids=%zu\n",
                ops, static_cast<long long>(global_alive), all_guids.size());
    }

    // ---- 8. guid 全局唯一（100 万次无重复，防复制基础） ----
    {
        std::vector<ItemGuid> v;
        v.reserve(1000000);
        ItemGuid prev = 0;
        bool ok = true;
        for (int i = 0; i < 1000000; ++i) {
            const ItemGuid g = NewItemGuid();
            if (g == 0) ok = false;            // 0 保留为无效
            if (i > 0 && g <= prev) ok = false;  // 严格递增 ⇒ 唯一
            prev = g;
            v.push_back(g);
        }
        CHECK(ok);
        std::sort(v.begin(), v.end());
        for (std::size_t i = 1; i < v.size(); ++i) CHECK(v[i] != v[i - 1]);
        LineFmt("guid uniqueness: 1000000 guids, distinct=%d\n", ok ? 1 : 0);
    }

    if (g_fail == 0) {
        LineFmt("Inventory.Suite: ALL PASS\n");
        return 0;
    }
    ErrorFmt("Inventory.Suite: %d FAIL\n", g_fail);
    return 1;
}
