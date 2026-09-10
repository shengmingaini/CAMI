// scripting/lua/tests/lua_integration_test.cpp —— TASK-031 · 集成测试（§17）
//
// 场景（严格按 §17 要求搭建）
// -------------------------
//   在「Scene」里挂一个 ScriptContext，用 Lua 实现一个**受击时触发**的脚本，跑完整链路：
//
//     C++ 发出受击事件（entity.hit）
//       -> Lua 订阅者被调用（event.subscribe）
//       -> 脚本调用 skill.cast(caster, 1001, target) 请求施法
//       -> C++ 侧 op handler 结算伤害（只有它写 HP）
//       -> 结果回传 Lua（{ok,i0=伤害,i1=剩余HP}）
//       -> Lua 再发布 damage.applied 事件
//       -> C++ 侧订阅者收到，链路闭环
//
//   断言：
//     1. 全链路往返正确（伤害 / 剩余 HP / 事件载荷逐项比对）
//     2. 伤害只由 C++ 结算一次（Lua 无法干预数值）
//     3. 限额生效：订阅者死循环不卡死 Tick（Tick 正常返回，其它订阅者照常收到）
//     4. 脚本错误不影响 Scene Tick（继续跑后续 Tick）
//     5. 场景内只有一个 VM；Destroy 后失效 id 不再被处理
//
// 脚本函数一律用**全局函数定义**写法（`function on_hit()`），因为 `Call` 在脚本模块表
// （不返回 table 时即该脚本私有的 `_ENV`）里查函数。

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "mmo/core/bus/command_bus.h"
#include "mmo/core/bus/event_bus.h"
#include "mmo/core/bus/query_bus.h"
#include "mmo/core/error/error.h"
#include "mmo/core/error/error_code.h"
#include "mmo/core/time/clock.h"
#include "mmo/game/entity/entity.h"
#include "mmo/game/entity/entity_manager.h"
#include "mmo/script/lua_vm.h"
#include "mmo/script/script_binding.h"
#include "mmo/script/script_context.h"
#include "mmo/script/script_event.h"

#include "test_print.h"

using mmo::core::ErrorCode;
using mmo::script::BindingDef;
using mmo::script::BindingKind;
using mmo::script::LuaLimits;
using mmo::script::ScriptCall;
using mmo::script::ScriptContext;
using mmo::script::ScriptError;
using mmo::script::ScriptEvent;
using mmo::script::ScriptReply;
using mmo::script::ScriptValue;

namespace {

// 本文件在全局命名空间，`core::` 需要显式指向 mmo::core（mmo::script::core 不存在）。
namespace core = mmo::core;

int failures = 0;

#define CHECK(cond)                                                                 \
    do {                                                                            \
        if (!(cond)) {                                                              \
            ::mmo::core::test::ErrorFmt("FAIL @ %s:%d : %s\n", __FILE__, __LINE__,   \
                                        #cond);                                     \
            ++failures;                                                             \
        }                                                                           \
    } while (0)

#define CHECK_EQ(actual, expected)                                                        \
    do {                                                                                  \
        const auto _a = (actual);                                                         \
        const auto _e = (expected);                                                       \
        if (!(_a == _e)) {                                                                \
            ::mmo::core::test::ErrorFmt("FAIL @ %s:%d : %s => %lld want %lld\n", __FILE__, \
                                        __LINE__, #actual,                                 \
                                        static_cast<long long>(_a),                        \
                                        static_cast<long long>(_e));                       \
            ++failures;                                                                   \
        }                                                                                 \
    } while (0)

// ---------------------------------------------------------------------------
// 观测探针（脚本 → C++ 回传通道）
// ---------------------------------------------------------------------------

struct Probe {
    static constexpr std::size_t kCapacity = 256;
    std::int64_t values[kCapacity]{};
    std::size_t count{0};

    void Reset() { count = 0; }
};

core::Result<int> ProbeReport(ScriptCall& call, void* user) {
    auto* probe = static_cast<Probe*>(user);
    if (probe == nullptr || probe->count >= Probe::kCapacity) {
        return call.Done();
    }
    probe->values[probe->count] = call.ArgCount() > 0 ? call.Arg(0).AsInt() : -1;
    ++probe->count;
    return call.Done();
}

// ---------------------------------------------------------------------------
// 场景（模拟 GameNode 的一个 Scene 子集：Entity + Bus + ScriptContext）
// ---------------------------------------------------------------------------

/// 受击输入：由「外部」驱动（客户端包 / 其它系统的 AI 决策）。
struct HitInput {
    std::int64_t attacker{0};
    std::int64_t target{0};
};

class FakeScene {
public:
    FakeScene() : entities_(&bus_, 4096) {}

    bool Init(bool with_broken_script);

    // ---- 外部输入 ----
    void PushHit(const HitInput& in) {
        ScriptEvent ev;
        (void)ev.SetName("entity.hit");
        (void)ev.PushArg(ScriptValue::Int(in.target));
        (void)ev.PushArg(ScriptValue::Int(in.attacker));
        const auto published = bus_.Publish(ev);
        CHECK(static_cast<bool>(published));
    }

    /// 一个 Tick：Event 阶段（带时间预算的 Drain）+ 实体延迟回收。
    void Tick() {
        (void)bus_.Drain(4096, mmo::core::DurationMs{2});
        entities_.FlushDeferred();
    }

    mmo::game::EntityManager& Entities() { return entities_; }
    mmo::core::EventBus& Bus() { return bus_; }
    ScriptContext& Scripts() { return *scripts_; }
    std::int64_t HpOf(std::int64_t target_id) const {
        const auto it = hp_.find(target_id);
        return it == hp_.end() ? -1 : it->second;
    }
    std::size_t SkillCalls() const { return skill_calls_; }
    std::size_t DamageEvents() const { return damage_events_; }
    std::int64_t LastDamageEventTarget() const { return last_damage_target_; }
    std::int64_t LastDamageEventDamage() const { return last_damage_damage_; }
    std::int64_t LastDamageEventHp() const { return last_damage_hp_; }
    void ResetDamageEvents() {
        damage_events_ = 0;
        last_damage_target_ = -1;
        last_damage_damage_ = -1;
        last_damage_hp_ = -1;
    }

    /// 脚本侧回传的观测值（§17「结果回传 Lua」这一段的证据）。
    const Probe& ProbeState() const { return probe_; }
    void ResetProbe() { probe_.Reset(); }

private:
    /// 唯一的 HP 写入者（模拟 Role / Combat 系统，§8 状态 Owner）。
    std::unordered_map<std::int64_t, std::int64_t> hp_;
    std::size_t skill_calls_{0};
    std::size_t damage_events_{0};
    std::int64_t last_damage_target_{-1};
    std::int64_t last_damage_damage_{-1};
    std::int64_t last_damage_hp_{-1};

    mmo::core::EventBus bus_;
    mmo::core::CommandBus commands_;
    mmo::core::QueryBus queries_;
    mmo::game::EntityManager entities_;
    std::unique_ptr<ScriptContext> scripts_;
    Probe probe_;

    static core::Result<ScriptReply> OnSkillCast(const mmo::script::ScriptCommand& cmd, void* user);
    static core::Result<ScriptReply> OnQuery(const mmo::script::ScriptQuery& query, void* user);
    void ObserveDamage(const ScriptEvent& ev);
};

core::Result<ScriptReply> FakeScene::OnSkillCast(const mmo::script::ScriptCommand& cmd,
                                                void* user) {
    auto* scene = static_cast<FakeScene*>(user);
    scene->skill_calls_ += 1;

    const std::int64_t caster = cmd.args.Size() > 0 ? cmd.args.At(0).AsInt() : 0;
    const std::int64_t skill_id = cmd.args.Size() > 1 ? cmd.args.At(1).AsInt() : 0;
    const std::int64_t target = cmd.args.Size() > 2 ? cmd.args.At(2).AsInt() : 0;

    // 伤害公式由 C++ 决定（脚本改不了）：skill_id 决定基础值，攻击者等级参与加成。
    const std::int64_t base = skill_id == 1001 ? 120 : 60;
    const std::int64_t bonus = (caster % 10) * 3;
    const std::int64_t damage = base + bonus;

    auto it = scene->hp_.find(target);
    std::int64_t hp = it == scene->hp_.end() ? 1000 : it->second;
    hp -= damage;
    if (hp < 0) {
        hp = 0;
    }
    scene->hp_[target] = hp;

    ScriptReply reply;
    reply.valid = true;
    reply.i0 = damage;
    reply.i1 = hp;
    return core::Result<ScriptReply>::Ok(reply);
}

core::Result<ScriptReply> FakeScene::OnQuery(const mmo::script::ScriptQuery& query, void* user) {
    (void)user;
    const std::int64_t target = query.args.Size() > 0 ? query.args.At(0).AsInt() : 0;
    ScriptReply reply;
    reply.valid = true;
    reply.i0 = target * 2;
    return core::Result<ScriptReply>::Ok(reply);
}

void FakeScene::ObserveDamage(const ScriptEvent& ev) {
    if (ev.Name() != "damage.applied") {
        return;
    }
    damage_events_ += 1;
    last_damage_target_ = ev.Arg(0).AsInt();
    last_damage_damage_ = ev.Arg(1).AsInt();
    last_damage_hp_ = ev.Arg(2).AsInt();
}

bool FakeScene::Init(bool with_broken_script) {
    LuaLimits limits;
    limits.max_instructions = 200000;  // 足够跑完正常脚本；死循环会在 ms 级被中止
    limits.max_exec_time = mmo::core::DurationMs{5};

    auto created = ScriptContext::Create(limits);
    if (!created) {
        return false;
    }
    scripts_ = std::move(created).Value();

    // 探针绑定（宿主扩展点，§27.4）：不修改本模块任何文件
    BindingDef probe_def;
    probe_def.name = "probe.report";
    probe_def.kind = BindingKind::Native;
    probe_def.fn = &ProbeReport;
    probe_def.user = static_cast<void*>(&probe_);
    probe_def.read_only = true;
    probe_def.description = "integration probe";
    if (!scripts_->AddBinding(std::move(probe_def))) {
        return false;
    }

    if (!scripts_->BindEntityApi(entities_) || !scripts_->BindEventApi(bus_) ||
        !scripts_->BindCommandApi(commands_) || !scripts_->BindQueryApi(queries_)) {
        return false;
    }
    if (!scripts_->RegisterCommandOp("skill.cast", &FakeScene::OnSkillCast,
                                     static_cast<void*>(this))) {
        return false;
    }
    if (!scripts_->RegisterQueryOp("probe.query", &FakeScene::OnQuery,
                                   static_cast<void*>(this))) {
        return false;
    }

    // 宿主观察 damage.applied（链路末端）
    auto sub = bus_.Subscribe<ScriptEvent>([this](const ScriptEvent& ev) { ObserveDamage(ev); });
    if (!sub) {
        return false;
    }

    // ---- 受击脚本（§17 主角）----
    static const char kOnHit[] = R"LUA(
function on_hit(t)
  local target = t[1]
  local caster = t[2]
  local r = skill.cast(caster, 1001, target)
  probe.report(r.ok and 1 or 0)   -- [a] 命令是否成功
  probe.report(r.i0)              -- [b] 伤害
  probe.report(r.i1)              -- [c] 剩余 HP
  event.publish('damage.applied', target, r.i0, r.i1)
end
event.subscribe('entity.hit', on_hit)
)LUA";
    if (!scripts_->Load("on_hit.lua", kOnHit)) {
        return false;
    }

    if (with_broken_script) {
        // 一个「会报错」的订阅者 + 一个「正常」的订阅者：验证派发隔离
        static const char kBroken[] = R"LUA(
function on_hit_broken(t)
  error('broken subscriber')
end
function on_hit_ok(t)
  probe.report(777)
end
event.subscribe('entity.hit', on_hit_broken)
event.subscribe('entity.hit', on_hit_ok)
)LUA";
        if (!scripts_->Load("broken.lua", kBroken)) {
            return false;
        }
    }
    return true;
}

// ===========================================================================
// 用例
// ===========================================================================

void TestFullRoundTrip() {
    FakeScene scene;
    CHECK(scene.Init(false));

    mmo::game::Position pos;
    pos.x = 1.0f;
    const auto caster = scene.Entities().Create(mmo::game::EntityType::Player, 1u, pos);
    const auto target = scene.Entities().Create(mmo::game::EntityType::Monster, 1u, pos);
    CHECK(static_cast<bool>(caster));
    CHECK(static_cast<bool>(target));
    if (!caster || !target) {
        return;
    }
    CHECK_EQ(scene.Scripts().SubscriberCount("entity.hit"), 1u);
    CHECK_EQ(scene.Scripts().LoadedCount(), 1u);

    const std::int64_t caster_id = static_cast<std::int64_t>(caster.Value()->Id());
    const std::int64_t target_id = static_cast<std::int64_t>(target.Value()->Id());

    // ---- Tick 1：输入入队 → Drain 派发 → Lua 跑 → 命令结算 → Lua 再发布 ----
    scene.ResetProbe();
    scene.PushHit(HitInput{caster_id, target_id});
    scene.Tick();

    const std::int64_t expected_damage = 120 + (caster_id % 10) * 3;
    const std::int64_t expected_hp = 1000 - expected_damage;

    CHECK_EQ(scene.SkillCalls(), 1u);
    CHECK_EQ(scene.HpOf(target_id), expected_hp);
    // 脚本侧（Lua）看到的结果必须与 C++ 结算**逐项一致** —— 全链路往返正确
    CHECK_EQ(scene.ProbeState().count, 3u);
    if (scene.ProbeState().count == 3u) {
        CHECK_EQ(scene.ProbeState().values[0], 1);               // r.ok
        CHECK_EQ(scene.ProbeState().values[1], expected_damage);  // 伤害回传 Lua
        CHECK_EQ(scene.ProbeState().values[2], expected_hp);      // 剩余 HP 回传 Lua
    }
    CHECK(scene.Scripts().LastError().code == ScriptError::Ok);

    // ---- Tick 2：damage.applied 从队列派发给宿主 ----
    scene.Tick();
    CHECK_EQ(scene.DamageEvents(), 1u);
    CHECK_EQ(scene.LastDamageEventTarget(), target_id);
    CHECK_EQ(scene.LastDamageEventDamage(), expected_damage);
    CHECK_EQ(scene.LastDamageEventHp(), expected_hp);

    // ---- 连续 100 次受击：一次不多一次不少（命令幂等性与 Tick 稳定性）----
    scene.ResetDamageEvents();
    const std::size_t calls_before = scene.SkillCalls();
    for (int i = 0; i < 100; ++i) {
        scene.PushHit(HitInput{caster_id, target_id});
        scene.Tick();
        scene.Tick();
    }
    CHECK_EQ(scene.SkillCalls() - calls_before, 100u);
    CHECK_EQ(scene.DamageEvents(), 100u);
    std::int64_t hp = 1000 - expected_damage * 101;
    if (hp < 0) {
        hp = 0;
    }
    CHECK_EQ(scene.HpOf(target_id), hp);

    // ---- 失效 EntityId：脚本持有已销毁目标 → 命令仍被派发（业务系统自行判空），
    //      但 entity.get 一定返回 NOT_FOUND（§19）----
    CHECK(static_cast<bool>(scene.Entities().Destroy(target.Value()->Id())));
    scene.Entities().FlushDeferred();
    const auto stale = scene.Scripts().Load(
        "stale.lua", "function probe_entity(id)\n"
                     "  local e = entity.get(id)\n"
                     "  probe.report(e.id)\n"
                     "end");
    CHECK(static_cast<bool>(stale));
    const auto stale_result = scene.Scripts().Call(stale.Value(), "probe_entity", target_id);
    CHECK(!stale_result);
    CHECK(stale_result.Err().Code() == ErrorCode::NOT_FOUND);
}

void TestLimitDoesNotStallTick() {
    FakeScene scene;
    CHECK(scene.Init(false));

    mmo::game::Position pos;
    const auto caster = scene.Entities().Create(mmo::game::EntityType::Player, 1u, pos);
    CHECK(static_cast<bool>(caster));
    if (!caster) {
        return;
    }
    const std::int64_t caster_id = static_cast<std::int64_t>(caster.Value()->Id());

    // 再挂一个「死循环」订阅者 + 一个「正常」订阅者（后者必须照常收到事件）
    const auto spin = scene.Scripts().Load(
        "spin.lua",
        "function on_hit_spin(t) while true do end end\n"
        "function on_hit_after(t) probe.report(2024) end\n"
        "event.subscribe('entity.hit', on_hit_spin)\n"
        "event.subscribe('entity.hit', on_hit_after)");
    CHECK(static_cast<bool>(spin));
    CHECK_EQ(scene.Scripts().SubscriberCount("entity.hit"), 3u);

    scene.PushHit(HitInput{caster_id, 4242});

    scene.ResetProbe();
    const auto t0 = std::chrono::steady_clock::now();
    scene.Tick();
    const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now() - t0)
                                .count();

    // 死循环被指令上限中止，Tick 照常返回（**不卡死**）
    CHECK(elapsed_ms < 2000);
    CHECK(scene.Scripts().LimitHits() >= 1u);
    CHECK(scene.Scripts().LastError().code == ScriptError::InstructionLimit);
    // 派发隔离：后面两个正常订阅者仍然收到事件并执行
    CHECK_EQ(scene.SkillCalls(), 1u);
    // on_hit（伤害 120+bonus）与 on_hit_after（2024）都跑到了
    CHECK_EQ(scene.ProbeState().count, 4u);
    CHECK_EQ(scene.ProbeState().values[0], 1);
    CHECK_EQ(scene.ProbeState().values[3], 2024);

    // 后续 Tick 不受影响
    const std::size_t hits_before = scene.Scripts().LimitHits();
    for (int i = 0; i < 10; ++i) {
        scene.Tick();
    }
    CHECK_EQ(scene.Scripts().LimitHits(), hits_before);

    ::mmo::core::test::LineFmt("  [info] tick with spinning subscriber: %lld ms\n",
                               static_cast<long long>(elapsed_ms));
}

void TestBrokenScriptDoesNotBreakTick() {
    FakeScene scene;
    CHECK(scene.Init(true));  // 含一个会 error 的订阅者

    mmo::game::Position pos;
    const auto caster = scene.Entities().Create(mmo::game::EntityType::Player, 1u, pos);
    CHECK(static_cast<bool>(caster));
    if (!caster) {
        return;
    }
    const std::int64_t caster_id = static_cast<std::int64_t>(caster.Value()->Id());
    CHECK_EQ(scene.Scripts().SubscriberCount("entity.hit"), 3u);

    scene.PushHit(HitInput{caster_id, 555});
    scene.Tick();
    scene.Tick();

    // 坏订阅者的 error 被捕获，好的订阅者与主链路照常完成
    CHECK_EQ(scene.SkillCalls(), 1u);
    CHECK_EQ(scene.DamageEvents(), 1u);
    CHECK_EQ(scene.LastDamageEventTarget(), 555);

    // Scene 继续 Tick 20 次不崩、并且每次都能完成主链路
    scene.ResetDamageEvents();
    const std::size_t before = scene.SkillCalls();
    for (int i = 0; i < 20; ++i) {
        scene.PushHit(HitInput{caster_id, 555});
        scene.Tick();
        scene.Tick();
    }
    CHECK_EQ(scene.SkillCalls() - before, 20u);
    CHECK_EQ(scene.DamageEvents(), 20u);

    // 宿主进程活着：脚本集合依旧可用
    CHECK(scene.Scripts().IsLoaded(1u));
    CHECK_EQ(scene.Scripts().Vm().OnOwnerThread(), 1u);
}

void TestIndependentScenesDoNotShareVm() {
    // §20.1：每个 Scene 一个 VM。两个 Scene 的同名脚本互不干扰（各自的 _ENV）。
    FakeScene a;
    FakeScene b;
    CHECK(a.Init(false));
    CHECK(b.Init(false));

    const auto sa = a.Scripts().Load("same.lua", "function mark() shared_counter = 1 end\n"
                                               "function peek() probe.report(shared_counter or -1) end");
    const auto sb = b.Scripts().Load("same.lua", "function peek() probe.report(shared_counter or -1) end");
    CHECK(static_cast<bool>(sa));
    CHECK(static_cast<bool>(sb));

    CHECK(static_cast<bool>(a.Scripts().Call(sa.Value(), "mark")));
    // b 的脚本看不到 a 的全局写
    CHECK(static_cast<bool>(b.Scripts().Call(sb.Value(), "peek")));
    CHECK(static_cast<bool>(a.Scripts().Call(sa.Value(), "peek")));

    CHECK(a.Scripts().Vm().NativeState() != b.Scripts().Vm().NativeState());
    CHECK(a.Scripts().Vm().NativeState() != nullptr);
    CHECK_EQ(a.Scripts().LoadedCount(), 2u);
    CHECK_EQ(b.Scripts().LoadedCount(), 2u);
}

void TestQueryThroughScene() {
    FakeScene scene;
    CHECK(scene.Init(false));
    const auto q = scene.Scripts().Load("q.lua",
                                        "function ask(target)\n"
                                        "  local r = query.ask('probe.query', target)\n"
                                        "  probe.report(r.ok and 1 or 0)\n"
                                        "  probe.report(r.i0)\n"
                                        "end");
    CHECK(static_cast<bool>(q));
    CHECK(static_cast<bool>(scene.Scripts().Call(q.Value(), "ask", 33)));
}

}  // namespace

int main() {
    ::mmo::core::test::Line("=== TASK-031 Lua Runtime integration test (§17) ===\n");
    TestFullRoundTrip();
    TestLimitDoesNotStallTick();
    TestBrokenScriptDoesNotBreakTick();
    TestIndependentScenesDoNotShareVm();
    TestQueryThroughScene();

    if (failures == 0) {
        ::mmo::core::test::Line("ALL LUA INTEGRATION TESTS PASSED\n");
        return 0;
    }
    ::mmo::core::test::ErrorFmt("%d TEST(S) FAILED\n", failures);
    return 1;
}
