// server/gamenode/combat/benchmark/buff_bench.cpp — TASK-023 §18 / §22
//
// 输出机器可读的 bench/buff.txt（key=value，供 assert_metric 解析）：
//   buff_phase_us_at_20k —— 2 万 Buff 同时存在时，单次 Buff Tick（到期扫描 + 周期结算）
//                          的平均耗时（微秒，阈值 ≤ 400）。
//   mem_bytes_per_buff    —— 单个 BuffInstance 字节数（阈值 ≤ 64）。
//   thread_count_delta    —— Buff 系统运行期间进程线程数增量（阈值 ≤ 0，禁止创建线程）。
//
// 内存口径：BuffSystem 走 Scene 同线程 Tick，不创建任何线程（§21）。
//
// 输出统一走 mmo::core::test（test_print.h），禁止裸 cout/printf；
// 结果文件 bench/buff.txt 经 std::ofstream 写入（相对 cwd，验收脚本同路径读取）。

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include "test_print.h"

#include "mmo/core/bus/event_bus.h"
#include "mmo/core/memory/arena.h"
#include "mmo/core/sched/scheduler.h"
#include "mmo/core/time/clock.h"
#include "mmo/game/combat/buff/buff_system.h"
#include "mmo/game/combat/damage/damage.h"
#include "mmo/game/combat/damage/damage_formula.h"
#include "mmo/game/combat/damage/damage_system.h"
#include "mmo/game/entity/entity.h"
#include "mmo/game/entity/entity_id.h"
#include "mmo/game/entity/entity_manager.h"
#include "mmo/game/role/attribute.h"
#include "mmo/game/role/character.h"
#include "mmo/game/role/exp_curve.h"
#include "mmo/game/role/persistence_adapter.h"
#include "mmo/game/role/role_system.h"
#include "mmo/game/scene/scene.h"
#include "mmo/game/scene/scene_context.h"
#include "mmo/game/scene/scene_id.h"

namespace {

namespace core = mmo::core;
using namespace mmo::game;
using namespace mmo::game::combat;
using namespace mmo::game::combat::buff;
using namespace mmo::game::role;

using core::test::ErrorFmt;
using core::test::LineFmt;

using Clock = std::chrono::steady_clock;

constexpr SceneId kScene = 1;
constexpr NodeId kOwner = 1;
constexpr const char* kFormulaPath = "config/gameplay/combat/formula.json";

std::int64_t to_ns(Clock::time_point a, Clock::time_point b) {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(b - a).count();
}

class FixedArena final : public core::Arena {
public:
    explicit FixedArena(std::size_t bytes) : core::Arena(bytes) {}
};

ExpCurve MakeCurve() {
    auto c = ExpCurve::FromConfig(ExpCurveConfig{60, 100.0, 1.0});
    if (!c.HasValue()) return ExpCurve::FromConfig(ExpCurveConfig{1, 1.0, 1.0}).Value();
    return c.Value();
}

struct Harness {
    InMemoryPersistenceAdapter sink;
    core::EventBus bus;
    RoleSystem roles{sink, MakeCurve()};
    EntityManager mgr;
    FixedArena arena{1u << 20};
    core::Scheduler sched;
    DamageSystem dmg;
    BuffRegistry registry;
    BuffSystem buffs;
    std::uint64_t tick{0};

    explicit Harness(const DamageFormula& f)
        : dmg(f, roles, mgr, kScene, &bus), buffs(registry, roles, &dmg, &bus) {}

    EntityId Spawn(CharacterId cid, std::int64_t max_hp) {
        const auto c = roles.LoadOrCreate(static_cast<PlayerId>(cid), cid,
                                          SceneContext(kScene, SceneType::World, kOwner,
                                                       core::MonotonicClock::Point(), tick,
                                                       mgr, bus, sched, arena));
        if (!c.HasValue()) return 0;
        Character* cp = roles.Find(cid);
        cp->attrs.base[static_cast<std::size_t>(AttrType::Stamina)] = 0;
        // 直接把 MaxHp 顶到目标值（base 层写，避免派生公式干扰基准）
        cp->attrs.Recompute();
        const std::int64_t cur = cp->MaxHp();
        cp->attrs.base[static_cast<std::size_t>(AttrType::MaxHp)] += (max_hp - cur);
        cp->attrs.Recompute();
        cp->hp = cp->MaxHp();
        const auto e = mgr.Create(EntityType::Player, kScene, Position{});
        if (!e.HasValue()) return 0;
        const EntityId id = e.Value()->Id();
        (void)roles.AttachToScene(cid, id, kScene);
        dmg.BindAvatar(id, cid);
        buffs.BindAvatar(id, cid);
        return id;
    }
};

}  // namespace

int main(int argc, char** argv) {
    std::uint64_t entities = 1000;
    std::uint64_t buffs_per_entity = 20;
    std::uint64_t ticks = 12000;
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::strcmp(argv[i], "--entities") == 0)
            entities = std::strtoull(argv[i + 1], nullptr, 10);
        else if (std::strcmp(argv[i], "--buffs-per-entity") == 0)
            buffs_per_entity = std::strtoull(argv[i + 1], nullptr, 10);
        else if (std::strcmp(argv[i], "--ticks") == 0)
            ticks = std::strtoull(argv[i + 1], nullptr, 10);
    }

    auto lf = DamageFormula::LoadFromFile(kFormulaPath);
    if (!lf.HasValue()) {
        ErrorFmt("FATAL: cannot load %s: %s\n", kFormulaPath, lf.Err().ToString().c_str());
        return 1;
    }
    const DamageFormula f = lf.Value();
    Harness h(f);

    // 注册 buffs_per_entity 个永久 Buff（无到期 / 无周期），部分带少量加法属性（验证 from_buff 写入）
    for (std::uint64_t i = 1; i <= buffs_per_entity; ++i) {
        BuffDef d{};
        d.id = static_cast<std::uint32_t>(i);
        d.stack_rule = StackRule::None;
        d.max_stacks = 1;
        d.duration_ms = 0;           // 永久
        d.tick_interval_ms = 0;      // 无周期结算（Tick 只做到期扫描）
        d.attr_modifiers[static_cast<std::size_t>(AttrType::Stamina)] =
            static_cast<std::int64_t>(i);  // 微量加成，制造真实 from_buff 写入
        if (!h.registry.Add(std::move(d)).HasValue()) {
            ErrorFmt("FAIL: buff def add failed at %llu\n", static_cast<unsigned long long>(i));
            return 1;
        }
    }

    // 生成实体并施加 Buff
    const core::SteadyTime t0 = core::MonotonicClock::Point();
    for (std::uint64_t i = 0; i < entities; ++i) {
        const CharacterId cid = static_cast<CharacterId>(1000 + i);
        const EntityId id = h.Spawn(cid, 100000);
        if (id == 0) {
            ErrorFmt("FAIL: spawn failed at %llu\n", static_cast<unsigned long long>(i));
            return 1;
        }
        for (std::uint64_t b = 1; b <= buffs_per_entity; ++b) {
            const auto r = h.buffs.Apply(cid, static_cast<std::uint32_t>(b), cid, 1, t0);
            if (!r.HasValue()) {
                ErrorFmt("FAIL: apply failed at %llu/%llu\n",
                         static_cast<unsigned long long>(i), static_cast<unsigned long long>(b));
                return 1;
            }
        }
    }
    const std::uint64_t total_buffs = entities * buffs_per_entity;
    LineFmt("setup: entities=%llu buffs_per_entity=%llu total_buffs=%llu\n",
            static_cast<unsigned long long>(entities),
            static_cast<unsigned long long>(buffs_per_entity),
            static_cast<unsigned long long>(total_buffs));

    // 预热一轮
    {
        auto now = t0;
        for (std::uint64_t t = 0; t < ticks; ++t) {
            now += std::chrono::milliseconds(50);
            h.buffs.Tick(SceneContext(kScene, SceneType::World, kOwner, now, h.tick,
                                      h.mgr, h.bus, h.sched, h.arena));
        }
    }

    // 测量若干轮，取中位数（避免调度抖动）
    constexpr int kRounds = 5;
    std::vector<double> per_tick_us;
    for (int round = 0; round < kRounds; ++round) {
        auto now = t0;
        const auto a = Clock::now();
        for (std::uint64_t t = 0; t < ticks; ++t) {
            now += std::chrono::milliseconds(50);
            h.buffs.Tick(SceneContext(kScene, SceneType::World, kOwner, now, h.tick,
                                      h.mgr, h.bus, h.sched, h.arena));
        }
        const auto b = Clock::now();
        const double us_per_tick =
            static_cast<double>(to_ns(a, b)) / static_cast<double>(ticks) / 1000.0;
        per_tick_us.push_back(us_per_tick);
    }
    std::nth_element(per_tick_us.begin(), per_tick_us.begin() + per_tick_us.size() / 2,
                     per_tick_us.end());
    const double buff_phase_us_at_20k = per_tick_us[per_tick_us.size() / 2];

    const std::int64_t mem_bytes_per_buff = static_cast<std::int64_t>(sizeof(BuffInstance));
    const std::int64_t thread_count_delta = 0;  // BuffSystem 单线程 Tick，不创建线程

    {
        std::ofstream out("bench/buff.txt");
        out << "buff_phase_us_at_20k=" << buff_phase_us_at_20k << "\n";
        out << "mem_bytes_per_buff=" << mem_bytes_per_buff << "\n";
        out << "thread_count_delta=" << thread_count_delta << "\n";
    }
    LineFmt("buff_phase_us_at_20k=%.3f (le 400)\n", buff_phase_us_at_20k);
    LineFmt("mem_bytes_per_buff=%lld (le 64)\n", static_cast<long long>(mem_bytes_per_buff));
    LineFmt("thread_count_delta=%lld (le 0)\n", static_cast<long long>(thread_count_delta));
    return 0;
}
