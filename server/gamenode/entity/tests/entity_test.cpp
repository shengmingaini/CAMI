// server/gamenode/entity/tests/entity_test.cpp — TASK-011 §16 单元 / §17 集成 / §19 Failure
//
// 输出统一走 mmo::core::test（test_print.h），禁止裸 std::cout / printf。

#include <cstdint>
#include <cstdlib>
#include <functional>
#include <vector>

#include "test_print.h"
#include "mmo/core/bus/event_bus.h"
#include "mmo/game/entity/entity.h"
#include "mmo/game/entity/entity_events.h"
#include "mmo/game/entity/entity_manager.h"

namespace {

using namespace mmo::game;
using mmo::core::test::ErrorFmt;
using mmo::core::test::Line;
using mmo::core::test::LineFmt;
namespace core = mmo::core;

int g_fails = 0;

#define CHECK(cond, msg)                                                       \
    do {                                                                       \
        if (!(cond)) {                                                         \
            ErrorFmt("FAIL: %s (line %d)\n", msg, __LINE__);                   \
            ++g_fails;                                                         \
        }                                                                      \
    } while (0)
#define CHECK_CODE(result, expected, msg)                                      \
    do {                                                                       \
        const auto& r_ = (result);                                             \
        if (r_.HasValue()) {                                                   \
            ErrorFmt("FAIL: %s (line %d): expected error, got ok\n", msg,       \
                     __LINE__);                                                \
            ++g_fails;                                                         \
        } else if (r_.Err().Code() != (expected)) {                            \
            ErrorFmt("FAIL: %s (line %d): code mismatch\n", msg, __LINE__);    \
            ++g_fails;                                                         \
        }                                                                      \
    } while (0)
using core::ErrorCode;

// ---- 测试用组件（§17 集成 / §19 类型安全）----
class HealthComponent : public Component<HealthComponent> {
public:
    int hp = 100;
    int max_hp = 100;
};
class TagComponent : public Component<TagComponent> {
public:
    std::uint64_t value = 0;
};
// 故意「未挂载」的类型，用于验证 TryGet 类型安全返回 nullptr。
class OtherComponent : public Component<OtherComponent> {
public:
    std::uint64_t secret = 0;
};

std::uint64_t g_detached = 0;  // 记录 OnDetached 触发次数

// ---------------------------------------------------------------------------
// §16 EntityId 编解码
// ---------------------------------------------------------------------------
void TestEntityIdEncoding() {
    const EntityId id = MakeEntityId(12345u, 7u);
    CHECK(EntityIndex(id) == 12345u, "index decoded");
    CHECK(EntityGeneration(id) == 7u, "generation decoded");
    // 高位 32 = index，低位 32 = generation（§7 编码 (index<<32)|generation）
    CHECK((id >> 32) == 12345u, "high 32 = index");
    CHECK((id & 0xFFFFFFFFu) == 7u, "low 32 = generation");
    // 往返稳定
    CHECK(MakeEntityId(EntityIndex(id), EntityGeneration(id)) == id, "roundtrip");
}

// ---------------------------------------------------------------------------
// §16 / §20.1 SlotMap 复用与世代递增、ABA 防护
// ---------------------------------------------------------------------------
void TestSlotMapReuseAndGeneration() {
    core::EventBus bus;
    EntityManager mgr(&bus);

    auto r = mgr.Create(EntityType::Monster, 1, Position{});
    CHECK(r.HasValue(), "create ok");
    const EntityId first = r.Value()->Id();
    const std::uint32_t idx = EntityIndex(first);
    const std::uint32_t gen = EntityGeneration(first);

    // 销毁（延迟）→ Flush 物理回收
    CHECK(mgr.Destroy(first).HasValue(), "destroy ok");
    mgr.FlushDeferred();

    // 旧 id 必失效（防 ABA）
    CHECK(mgr.Find(first) == nullptr, "old id invalid after destroy");

    // 再次创建应复用同一槽位但世代 +1
    auto r2 = mgr.Create(EntityType::Monster, 1, Position{});
    CHECK(r2.HasValue(), "recreate ok");
    const EntityId second = r2.Value()->Id();
    CHECK(EntityIndex(second) == idx, "index reused");
    CHECK(EntityGeneration(second) == gen + 1u, "generation incremented");
    CHECK(second != first, "new id differs from old");
    CHECK(mgr.Find(second) != nullptr, "new id findable");
    CHECK(mgr.Find(first) == nullptr, "old id still invalid");
}

// ---------------------------------------------------------------------------
// §16 Create / Destroy / Find O(1) + §20.1 防 ABA
// ---------------------------------------------------------------------------
void TestCreateFindDestroy() {
    core::EventBus bus;
    EntityManager mgr(&bus);

    auto r = mgr.Create(EntityType::Player, 5, Position{});
    CHECK(r.HasValue(), "create");
    Entity* e = r.Value();
    CHECK(e->Id() != 0, "non-zero id");
    CHECK(e->Type() == EntityType::Player, "type");
    CHECK(e->Scene() == 5, "scene");
    CHECK(e->Alive(), "alive");
    CHECK(mgr.Find(e->Id()) == e, "find == create");
    CHECK(mgr.AliveCount() == 1, "alive count");

    CHECK(mgr.Destroy(e->Id()).HasValue(), "destroy");
    CHECK(mgr.Find(e->Id()) == nullptr, "find after destroy -> nullptr");
    mgr.FlushDeferred();
    CHECK(mgr.AliveCount() == 0, "alive count 0 after flush");
}

// ---------------------------------------------------------------------------
// §16 / §19 组件增删查 + 类型安全
// ---------------------------------------------------------------------------
void TestComponents() {
    core::EventBus bus;
    EntityManager mgr(&bus);
    auto r = mgr.Create(EntityType::Npc, 1, Position{});
    CHECK(r.HasValue(), "create");
    Entity* e = r.Value();

    HealthComponent* h = e->AddComponent<HealthComponent>();
    CHECK(h != nullptr, "add HealthComponent");
    CHECK(e->TryGet<HealthComponent>() == h, "TryGet returns same");
    CHECK(h->hp == 100, "default hp");

    TagComponent* t = e->AddComponent<TagComponent>();
    CHECK(t != nullptr, "add TagComponent");
    CHECK(e->TryGet<HealthComponent>() != nullptr, "both present");
    CHECK(e->TryGet<TagComponent>() == t, "tag present");

    // 类型安全：未挂载类型返回 nullptr
    CHECK(e->TryGet<OtherComponent>() == nullptr, "unattached type -> nullptr");
    const Entity* ce = e;
    CHECK(ce->TryGet<HealthComponent>() != nullptr, "const TryGet ok");
    CHECK(ce->TryGet<OtherComponent>() == nullptr, "const TryGet unattached -> nullptr");

    // 重复 AddComponent 同类型：幂等返回既有（不新建）
    HealthComponent* h2 = e->AddComponent<HealthComponent>();
    CHECK(h2 == h, "re-AddComponent idempotent");

    // 卸载
    CHECK(e->RemoveComponent<HealthComponent>(), "remove HealthComponent");
    CHECK(e->TryGet<HealthComponent>() == nullptr, "after remove -> nullptr");
    CHECK(e->TryGet<TagComponent>() != nullptr, "tag still present");
    CHECK(!e->RemoveComponent<OtherComponent>(), "remove unattached -> false");
}

// ---------------------------------------------------------------------------
// §16 Each 遍历完整性（§8 局部性好）
// ---------------------------------------------------------------------------
void TestEachCompleteness() {
    core::EventBus bus;
    EntityManager mgr(&bus);
    constexpr std::size_t kN = 2000;
    std::vector<EntityId> ids;
    ids.reserve(kN);
    for (std::size_t i = 0; i < kN; ++i) {
        auto r = mgr.Create(EntityType::Item, 1, Position{});
        CHECK(r.HasValue(), "create item");
        r.Value()->AddComponent<TagComponent>()->value = i;
        ids.push_back(r.Value()->Id());
    }
    CHECK(mgr.Count(EntityType::Item) == kN, "count");

    std::size_t visited = 0;
    std::uint64_t sum = 0;
    mgr.Each<TagComponent>([&](Entity&, TagComponent& c) {
        ++visited;
        sum += c.value;
    });
    CHECK(visited == kN, "Each visited all");
    // sum 0..kN-1 = kN*(kN-1)/2
    const std::uint64_t expect = kN * (kN - 1) / 2;
    CHECK(sum == expect, "Each values complete & intact");

    // 卸载部分后再 Each
    for (std::size_t i = 0; i < kN; i += 2) {
        (void)mgr.Destroy(ids[i]);
    }
    mgr.FlushDeferred();
    std::size_t visited2 = 0;
    mgr.Each<TagComponent>([&](Entity&, TagComponent&) { ++visited2; });
    CHECK(visited2 == kN / 2, "Each after partial destroy");
}

// ---------------------------------------------------------------------------
// §15.7 / §19 延迟销毁语义
// ---------------------------------------------------------------------------
void TestDeferredDestroySemantics() {
    core::EventBus bus;
    EntityManager mgr(&bus);
    auto r = mgr.Create(EntityType::Player, 1, Position{});
    Entity* e = r.Value();
    const EntityId id = e->Id();

    // Destroy 后逻辑立即死亡
    CHECK(mgr.Destroy(id).HasValue(), "destroy");
    CHECK(mgr.Find(id) == nullptr, "Find immediately nullptr after Destroy");
    CHECK(mgr.DeferredCount() == 1, "deferred queued");
    CHECK(mgr.AliveCount() == 1, "alive count unchanged until flush");

    // Flush 后才物理回收
    mgr.FlushDeferred();
    CHECK(mgr.DeferredCount() == 0, "deferred drained");
    CHECK(mgr.AliveCount() == 0, "alive count 0 after flush");
    CHECK(mgr.Capacity() >= 1, "slot still allocated (reusable)");
}

// ---------------------------------------------------------------------------
// §17 1 万实体混合压力 + 1000 Tick 遍历（无泄漏 / 无悬垂）
// ---------------------------------------------------------------------------
void TestStressMixed() {
    core::EventBus bus;
    EntityManager mgr(&bus);
    constexpr std::size_t kN = 10000;
    std::vector<EntityId> ids;
    ids.reserve(kN);

    for (std::size_t i = 0; i < kN; ++i) {
        const EntityType t = static_cast<EntityType>(1u + static_cast<std::uint8_t>(i % 6));
        auto r = mgr.Create(t, static_cast<SceneId>(i % 16 + 1), Position{});
        CHECK(r.HasValue(), "create mixed");
        Entity* e = r.Value();
        e->AddComponent<TagComponent>()->value = i;
        if (i % 3 == 0) {
            e->AddComponent<HealthComponent>()->hp = 50;
        }
        ids.push_back(e->Id());
    }
    CHECK(mgr.AliveCount() == kN, "all alive");

    // 1000 个 Tick：每个 Tick 遍历全部 Tag 组件（模拟系统更新）
    std::size_t ticks = 0;
    for (std::size_t tick = 0; tick < 1000; ++tick) {
        std::size_t n = 0;
        mgr.Each<TagComponent>([&](Entity&, TagComponent&) { ++n; });
        if (n != kN) {
            ErrorFmt("FAIL: tick %zu visited %zu != %zu\n", tick, n, kN);
            ++g_fails;
            break;
        }
        ++ticks;
    }
    CHECK(ticks == 1000, "1000 ticks all complete");

    // 全部延迟销毁 + Flush
    for (std::size_t i = 0; i < kN; ++i) {
        CHECK(mgr.Destroy(ids[i]).HasValue(), "destroy all");
    }
    mgr.FlushDeferred();
    CHECK(mgr.AliveCount() == 0, "all destroyed");
    CHECK(mgr.DeferredCount() == 0, "no leak in deferred");
}

// ---------------------------------------------------------------------------
// §19 Failure：重复 Destroy / 访问已销毁 / 超上限
// ---------------------------------------------------------------------------
void TestFailureDuplicateDestroy() {
    core::EventBus bus;
    EntityManager mgr(&bus);
    auto r = mgr.Create(EntityType::Player, 1, Position{});
    const EntityId id = r.Value()->Id();
    CHECK(mgr.Destroy(id).HasValue(), "first destroy ok");
    CHECK_CODE(mgr.Destroy(id), ErrorCode::NOT_FOUND, "repeat destroy -> NOT_FOUND");
    mgr.FlushDeferred();
    CHECK_CODE(mgr.Destroy(id), ErrorCode::NOT_FOUND, "destroy after flush -> NOT_FOUND");
}

void TestFailureCapReached() {
    core::EventBus bus;
    EntityManager mgr(&bus, 100);  // 上限 100
    std::size_t ok = 0;
    for (std::size_t i = 0; i < 100; ++i) {
        if (mgr.Create(EntityType::Player, 1, Position{}).HasValue()) {
            ++ok;
        }
    }
    CHECK(ok == 100, "created up to cap");
    CHECK_CODE(mgr.Create(EntityType::Player, 1, Position{}), ErrorCode::BUSY,
               "over cap -> BUSY not OOM");
}

void TestFailureTickMidDestroyNoDangle() {
    core::EventBus bus;
    EntityManager mgr(&bus);
    auto r = mgr.Create(EntityType::Monster, 1, Position{});
    Entity* e = r.Value();
    e->AddComponent<HealthComponent>();
    const EntityId id = e->Id();

    // Tick 中途 Destroy：逻辑死亡，Find 立即返回 nullptr（不可达旧内存）
    CHECK(mgr.Destroy(id).HasValue(), "mid-tick destroy");
    CHECK(mgr.Find(id) == nullptr, "stale id unreachable (no dangling via manager)");

    // Flush 在 Tick 边界执行：复用槽位，新 id 与旧 id 不冲突
    mgr.FlushDeferred();
    auto r2 = mgr.Create(EntityType::Monster, 1, Position{});
    CHECK(r2.HasValue(), "reuse after flush");
    CHECK(r2.Value()->Id() != id, "new id distinct");
    CHECK(mgr.Find(id) == nullptr, "old id still invalid (ABA protected)");
}

// ---------------------------------------------------------------------------
// §17 / §15.6 生命周期事件（走 EventBus，需 Drain 派发）
// ---------------------------------------------------------------------------
void TestLifecycleEvents() {
    core::EventBus bus;
    EntityManager mgr(&bus);

    std::size_t created = 0, destroyed = 0, attached = 0, detached = 0;
    EntityId created_id = 0, destroyed_id = 0, attached_id = 0, detached_id = 0;
    EntityType created_type = EntityType::Player;
    ComponentTypeId attached_type{}, detached_type{};

    (void)bus.Subscribe<EntityCreated>(
        [&](const EntityCreated& ev) { ++created; created_id = ev.id; created_type = ev.type; });
    (void)bus.Subscribe<EntityDestroyed>(
        [&](const EntityDestroyed& ev) { ++destroyed; destroyed_id = ev.id; });
    (void)bus.Subscribe<ComponentAttached>(
        [&](const ComponentAttached& ev) { ++attached; attached_id = ev.id; attached_type = ev.type; });
    (void)bus.Subscribe<ComponentDetached>(
        [&](const ComponentDetached& ev) { ++detached; detached_id = ev.id; detached_type = ev.type; });

    auto r = mgr.Create(EntityType::Player, 3, Position{});
    const EntityId id = r.Value()->Id();
    (void)bus.Drain();
    CHECK(created == 1, "EntityCreated fired");
    CHECK(created_id == id, "created id matches");
    CHECK(created_type == EntityType::Player, "created type matches");

    r.Value()->AddComponent<HealthComponent>();
    (void)bus.Drain();
    CHECK(attached == 1, "ComponentAttached fired");
    CHECK(attached_id == id, "attached id matches");
    CHECK(attached_type == ComponentTypeIdFor<HealthComponent>(), "attached type matches");

    r.Value()->RemoveComponent<HealthComponent>();
    (void)bus.Drain();
    CHECK(detached == 1, "ComponentDetached fired");
    CHECK(detached_id == id, "detached id matches");
    CHECK(detached_type == ComponentTypeIdFor<HealthComponent>(), "detached type matches");

    CHECK(mgr.Destroy(id).HasValue(), "destroy for event");
    mgr.FlushDeferred();
    (void)bus.Drain();
    CHECK(destroyed == 1, "EntityDestroyed fired");
    CHECK(destroyed_id == id, "destroyed id matches");
}

}  // namespace

int main() {
    Line("== TASK-011 entity test ==\n");

    TestEntityIdEncoding();
    TestSlotMapReuseAndGeneration();
    TestCreateFindDestroy();
    TestComponents();
    TestEachCompleteness();
    TestDeferredDestroySemantics();
    TestStressMixed();
    TestFailureDuplicateDestroy();
    TestFailureCapReached();
    TestFailureTickMidDestroyNoDangle();
    TestLifecycleEvents();

    if (g_fails == 0) {
        Line("ALL PASS\n");
        return 0;
    }
    ErrorFmt("FAILED: %d check(s)\n", g_fails);
    return 1;
}
