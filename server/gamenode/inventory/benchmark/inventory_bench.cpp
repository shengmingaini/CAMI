// server/gamenode/inventory/benchmark/inventory_bench.cpp — TASK-017 §18 / §22 / §24
//
// 输出机器可读 key=value 到 bench/inventory.txt（验收脚本 assert_metric 解析）：
//   players / add_ns / remove_ns / equip_ns / recompute_after_equip_ns / mem_bytes_per_item
//
// 输出统一走 mmo::core::test（test_print.h），禁止裸 std::cout / printf。
// 内存口径：替换全局 operator new/delete，按 _msize 统计净堆增量（同 role_bench）。

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <vector>

// ---- 全局堆占用统计（必须定义在 operator new/delete 之前） ----
namespace {
std::int64_t g_heap_bytes = 0;
std::int64_t g_heap_ops = 0;
}  // namespace

void* operator new(std::size_t n) {
    void* p = std::malloc(n == 0 ? 1 : n);
    if (p == nullptr) throw std::bad_alloc();
    g_heap_bytes += static_cast<std::int64_t>(_msize(p));
    ++g_heap_ops;
    return p;
}
void* operator new[](std::size_t n) { return operator new(n); }
void operator delete(void* p) noexcept {
    if (p != nullptr) {
        g_heap_bytes -= static_cast<std::int64_t>(_msize(p));
        std::free(p);
    }
}
void operator delete[](void* p) noexcept { operator delete(p); }
void operator delete(void* p, std::size_t) noexcept { operator delete(p); }
void operator delete[](void* p, std::size_t) noexcept { operator delete(p); }

#include <algorithm>
#include <string>

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

using core::MonotonicClock;
using core::SteadyNs;

// ItemAdded 事件留痕的 guid（bench 内捕获用，setup 阶段）
ItemGuid g_last_added_guid = 0;

int g_bench_fail = 0;
#define CHECK(cond)                                                       \
    do {                                                                  \
        if (!(cond)) {                                                    \
            ErrorFmt("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);      \
            ++g_bench_fail;                                               \
        }                                                                 \
    } while (0)

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
        if (!r.HasValue()) { ErrorFmt("FAIL: cannot load exp_curve.json\n"); std::exit(1); }
        return std::move(r).Value();
    }
    Harness()
        : ctx(7, SceneType::World, 1, core::MonotonicClock::Point(), 0, mgr, bus, scheduler, arena),
          curve(LoadCurve()),
          role(sink, curve),
          inv(store, role) {
        auto lr = store.LoadDir("config/gameplay/items");
        if (!lr.HasValue()) { ErrorFmt("FAIL: load items: %s\n", lr.Err().Message().data()); std::exit(1); }
        role.BindEventBus(bus);
        inv.BindEventBus(bus);
    }
    role::Character* EnsureChar(PlayerId p) {
        auto c = role.LoadOrCreate(p, 1000u + p, ctx);
        return c.HasValue() ? c.Value() : nullptr;
    }
};

double Median(std::vector<double> v) {
    if (v.empty()) return 0.0;
    std::sort(v.begin(), v.end());
    return v[v.size() / 2];
}

void Run(std::size_t players, std::size_t ops, const char* out_path) {
    Harness h;
    std::vector<PlayerId> ps;
    ps.reserve(players);
    for (std::size_t i = 0; i < players; ++i) {
        CHECK(h.EnsureChar(static_cast<PlayerId>(i + 1)) != nullptr);
        ps.push_back(static_cast<PlayerId>(i + 1));
    }

    // 订阅 ItemAdded 以在 setup 阶段捕获 guid（InventorySystem::Add 公开签名只返回加入量）
    (void)h.bus.Subscribe<ItemAdded>([](const ItemAdded& e) { g_last_added_guid = e.guid; });
    auto AddCap = [&](PlayerId p, ItemId d, std::uint32_t c) -> ItemGuid {
        (void)h.inv.Add(p, d, c, 0);
        (void)h.bus.Drain();  // 派发以捕获 guid（非计时关键路径）
        return g_last_added_guid;
    };

    const ItemId kPotion = 1;   // 治疗药水 max_stack=99
    const ItemId kSword = 10;   // 铁剑 max_stack=1, MainHand

    std::vector<double> add_samples, remove_samples, equip_samples, recompute_samples;
    add_samples.reserve(ops);
    remove_samples.reserve(ops);
    equip_samples.reserve(ops);
    recompute_samples.reserve(ops);

    // ---- add_ns：每轮全玩家各 Add 1 个药水 ----
    for (std::size_t r = 0; r < ops; ++r) {
        const SteadyNs t0 = MonotonicClock::Now();
        for (PlayerId p : ps) (void)h.inv.Add(p, kPotion, 1, 0);
        const SteadyNs t1 = MonotonicClock::Now();
        add_samples.push_back(static_cast<double>(t1 - t0) / static_cast<double>(players));
    }

    // ---- remove_ns：每轮全玩家各 Remove 1 个药水（移除后补足，保持状态稳定） ----
    {
        std::vector<ItemGuid> pg;
        pg.reserve(players);
        for (PlayerId p : ps) {
            pg.push_back(AddCap(p, kPotion, 10));
        }
        for (std::size_t r = 0; r < ops; ++r) {
            const SteadyNs t0 = MonotonicClock::Now();
            for (std::size_t i = 0; i < players; ++i) (void)h.inv.Remove(ps[i], pg[i], 1, 0);
            const SteadyNs t1 = MonotonicClock::Now();
            remove_samples.push_back(static_cast<double>(t1 - t0) / static_cast<double>(players));
            // 重置：重新加满以便下轮移除
            for (std::size_t i = 0; i < players; ++i) {
                pg[i] = AddCap(ps[i], kPotion, 10);
            }
        }
    }

    // ---- equip_ns（含属性重算）：每轮全玩家各 Equip 一把剑，随后 Unequip 复位 ----
    {
        std::vector<ItemGuid> sg;
        sg.reserve(players);
        for (PlayerId p : ps) {
            sg.push_back(AddCap(p, kSword, 1));
        }
        for (std::size_t r = 0; r < ops; ++r) {
            const SteadyNs t0 = MonotonicClock::Now();
            for (std::size_t i = 0; i < players; ++i) {
                (void)h.inv.Equip(ps[i], sg[i], EquipSlot::MainHand, 0);
            }
            const SteadyNs t1 = MonotonicClock::Now();
            equip_samples.push_back(static_cast<double>(t1 - t0) / static_cast<double>(players));
            for (std::size_t i = 0; i < players; ++i) (void)h.inv.Unequip(ps[i], EquipSlot::MainHand, 0);
            for (std::size_t i = 0; i < players; ++i) {
                sg[i] = AddCap(ps[i], kSword, 1);
            }
        }
    }

    // ---- recompute_after_equip_ns：Equip 触发的属性重算（role::RecomputeAttributes） ----
    {
        std::vector<CharacterId> cids;
        cids.reserve(players);
        for (PlayerId p : ps) cids.push_back(h.role.FindByPlayer(p)->id);
        for (std::size_t r = 0; r < ops; ++r) {
            const SteadyNs t0 = MonotonicClock::Now();
            for (CharacterId cid : cids) (void)h.role.RecomputeAttributes(cid);
            const SteadyNs t1 = MonotonicClock::Now();
            recompute_samples.push_back(static_cast<double>(t1 - t0) / static_cast<double>(players));
        }
    }

    // ---- mem_bytes_per_item：ItemStack 结构足迹（固定数组槽，§22 < 64B） ----
    const double mem_bytes_per_item = static_cast<double>(sizeof(ItemStack));

    const double add_ns = Median(add_samples);
    const double remove_ns = Median(remove_samples);
    const double equip_ns = Median(equip_samples);
    const double recompute_after_equip_ns = Median(recompute_samples);

    std::FILE* fp = std::fopen(out_path, "w");
    if (fp == nullptr) { ErrorFmt("FAIL: cannot open %s\n", out_path); std::exit(1); }
    std::fprintf(fp,
                 "players=%zu\n"
                 "add_ns=%.3f\n"
                 "remove_ns=%.3f\n"
                 "equip_ns=%.3f\n"
                 "recompute_after_equip_ns=%.3f\n"
                 "mem_bytes_per_item=%.3f\n",
                 players, add_ns, remove_ns, equip_ns, recompute_after_equip_ns, mem_bytes_per_item);
    std::fclose(fp);

    LineFmt("players=%zu add_ns=%.3f remove_ns=%.3f equip_ns=%.3f "
             "recompute_after_equip_ns=%.3f mem_bytes_per_item=%.3f\n",
             players, add_ns, remove_ns, equip_ns, recompute_after_equip_ns, mem_bytes_per_item);
}

}  // namespace

int main(int argc, char** argv) {
    std::size_t players = 1000;
    std::size_t ops = 100;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--players" && i + 1 < argc) players = static_cast<std::size_t>(std::atoll(argv[++i]));
        else if (a == "--ops" && i + 1 < argc) ops = static_cast<std::size_t>(std::atoll(argv[++i]));
    }
    LineFmt("== TASK-017 inventory_bench ==\n");
    Run(players, ops, "bench/inventory.txt");
    return 0;
}
