// scripting/gameplay/tests/gameplay_integration_test.cpp — TASK-033 · 端到端集成测试（§17）
//
// 目标：证明「玩法脚本层」与**真实业务系统**能串成一条链路，且每一步的状态归属正确（§4）。
//
// 装配形状（与未来 gamenode 装配层一致）
// -------------------------------------
//   EventBus ──(MonsterKilled / QuestCompleted 等)──► 装配层路由
//                                                      │
//                          ┌───────────────────────────┴──────────────┐
//                          ▼                                          ▼
//                   GameplayScriptHost（Lua）                  QuestSystem（状态 Owner）
//                          │  quest.set_progress / quest.complete
//                          └────────► op handler（本文件的适配器）────────►┘
//
//   **单一权威写入者**（§4）：任务进度只有 QuestSystem 能写；脚本只能经 `ScriptCommand`
//   提交「意图 + 标量参数」，由适配器翻译成 QuestSystem 的写入口（`OnEvent` / `TurnIn`）。
//
// 覆盖的端到端链路（§17）
// ---------------------
//   A. 击杀链：接任务 1001 → 3× MonsterKilled → Lua 推 3 次进度 → 目标达标 →
//      QuestSystem 发布 QuestCompleted → 装配层转发 → Lua `quest.complete` → 真实发奖（幂等）
//   B. 采集中途热更：接任务 1002 → 采 2 个 → **热更 collect_herbs** → 再采 3 个 → 5/5 达标
//      （证明热更后脚本仍在线、且任务进度不被重置）
//   C. 限时护送：`npc_talked` 启动倒计时（Lua on_tick 推进）→ `location_reached` 完成并发奖；
//      再来一次并让 dt 超时 → 脚本拒绝提交进度（限时任务超时自动失效）
//   D. 技能 → Boss：Lua 算伤害 → C++ 结算扣血 → 血量跨阈值 → Lua 判定切阶段 → C++ 执行广播/召唤
//   E. 热更实战演练：改火球公式 → 线上即时生效 → 回滚恢复（TASK-032 六阶段流水线）
//
// 输出统一走 mmo::core::test（test_print.h），禁止裸 std::cout / printf / cerr。
// 工作目录必须是仓库根（ctest 已设 WORKING_DIRECTORY = CMAKE_SOURCE_DIR）。

#include <cstddef>
#include <cstdint>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "mmo/core/bus/command_bus.h"
#include "mmo/core/bus/event_bus.h"
#include "mmo/core/error/error.h"
#include "mmo/core/error/error_code.h"
#include "mmo/core/log/trace_id.h"
#include "mmo/core/time/clock.h"
#include "mmo/game/entity/entity_manager.h"
#include "mmo/game/quest/quest_def.h"
#include "mmo/game/quest/quest_events.h"
#include "mmo/game/quest/quest_instance.h"
#include "mmo/game/quest/quest_system.h"
#include "mmo/gameplay/gameplay_script_host.h"
#include "mmo/gameplay/gameplay_types.h"
#include "mmo/script/script_event.h"
#include "mmo/script/script_value.h"

#include "test_print.h"

namespace {

namespace core = mmo::core;
namespace game = mmo::game;
namespace gp = mmo::gameplay;
namespace quest = mmo::game::quest;
namespace script = mmo::script;

using mmo::core::test::ErrorFmt;
using mmo::core::test::Line;
using mmo::core::test::LineFmt;

int failures = 0;
int checks = 0;

#define CHECK(cond)                                                     \
    do {                                                                \
        ++checks;                                                       \
        if (!(cond)) {                                                  \
            ErrorFmt("FAIL @ %s:%d : %s\n", __FILE__, __LINE__, #cond); \
            ++failures;                                                 \
        }                                                               \
    } while (0)

constexpr const char* kScriptManifest = "config/gameplay/scripts.json";
constexpr const char* kQuestConfigDir = "config/gameplay/quests";
constexpr const char* kFireballPath = "scripting/gameplay/skill/fireball.lua";
constexpr const char* kCollectHerbsPath = "scripting/gameplay/quest/collect_herbs.lua";

constexpr quest::PlayerId kPlayer = 42;
constexpr double kDragonMaxHp = 580.0;  // 让「一发火球」正好跨过 70% 阈值

std::string ReadFile(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return {};
    }
    std::ostringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
}

bool ReplaceOnce(std::string& text, std::string_view from, std::string_view to) {
    const std::size_t pos = text.find(from);
    if (pos == std::string::npos) {
        return false;
    }
    text.replace(pos, from.size(), to);
    return true;
}

// ===========================================================================
// 上游依赖的测试替身（§27.2：只消费公开接口，不碰内部状态）
// ===========================================================================

/// 玩家等级（TASK-016 Role 的角色，这里只需要 LevelOf）。
class LevelMock final : public quest::IPlayerQuery {
public:
    std::uint32_t LevelOf(quest::PlayerId) const noexcept override { return level; }
    std::uint32_t level = 10;
};

/// 奖励发放记录器（TASK-029 Economy 就绪前的占位实现，签名与最终一致）。
class RewardLog final : public quest::QuestSystem::IRewardSink {
public:
    core::Result<void> Grant(quest::PlayerId player, const quest::QuestDef& def,
                             core::TraceID) override {
        ++grants;
        last_player = player;
        last_quest = def.id;
        last_exp = def.exp_reward;
        last_currency = def.currency_reward;
        return core::Result<void>::Ok();
    }

    std::size_t grants{0};
    quest::PlayerId last_player{0};
    quest::QuestId last_quest{0};
    std::uint64_t last_exp{0};
    std::uint64_t last_currency{0};
};

/// 外部引用注册表（§19「脚本引用的 Quest/Boss 不存在 → 加载期校验报错」）。
class ReferenceMock final : public gp::IReferenceRegistry {
public:
    bool Exists(std::string_view ns, std::string_view id) const noexcept override {
        if (ns == "quest") {
            return id == "1001" || id == "1002" || id == "1004" || id == "1005";
        }
        if (ns == "npc") {
            return id == "1001" || id == "2002";
        }
        if (ns == "item") {
            return id == "9001" || id == "9002" || id == "9003";
        }
        if (ns == "skill") {
            return id == "fireball" || id == "ice_lance" || id == "heal_light";
        }
        if (ns == "ai") {
            return id == "aggressive_guard" || id == "patrol_sentry" || id == "fleeing_scout";
        }
        if (ns == "boss") {
            return id == "dragon" || id == "lich" || id == "golem";
        }
        if (ns == "activity") {
            return id == "double_exp" || id == "world_boss_dawn" || id == "festival_lantern";
        }
        return false;
    }
};

// ===========================================================================
// 装配层（脚本意图 → 业务系统写入口 的适配器）
// ===========================================================================

/// 「装配层」= 把玩法脚本层与业务系统接起来的那一层。
///
/// 职责边界（重要）：
///   - 脚本只表达「对第 idx 个目标推进 value」（`quest.set_progress(player, idx, value)`），
///     它**不认识** QuestDef —— 「idx → (目标类型, 目标 id)」的翻译是装配层职责。
///     真实装配层从清单的 `requires: quest:<id>` 得到脚本服务哪个任务；本测试用
///     `progress_quest` 显式指定（单任务会话），语义等价且可断言。
///   - 所有写入都发生在**状态 Owner 内部**（QuestSystem），脚本拿不到任何句柄。
class Assembly {
public:
    Assembly() : quests(&bus) {}

    bool Setup() {
        const core::Result<std::size_t> loaded_quests = quests.LoadQuests(kQuestConfigDir);
        if (!loaded_quests) {
            ErrorFmt("FAIL: LoadQuests: %s\n", std::string(loaded_quests.Err().Message()).c_str());
            return false;
        }
        quests.BindPlayerQuery(levels);
        quests.BindRewardSink(rewards);

        // QuestCompleted（QuestSystem 发布）→ 转发给玩法脚本层。
        // 这是「Lua 决定何时交还、QuestSystem 执行交还」的连接点。
        const core::Result<core::EventBus::SubId> sub =
            bus.Subscribe<quest::QuestCompleted>([this](const quest::QuestCompleted& ev) {
                gp::GameplayPayload payload;
                (void)payload.SetName("quest_completed");
                (void)payload.AddInt("quest", static_cast<std::int64_t>(ev.quest));
                (void)payload.AddInt("player", static_cast<std::int64_t>(ev.player));
                (void)host->DispatchQuestEvent(payload);
            });
        if (!sub) {
            ErrorFmt("FAIL: subscribe QuestCompleted\n");
            return false;
        }

        gp::GameplayScriptHost::Config config;
        config.root = "";
        config.scene_id = 1;
        config.require_all_scripts = true;
        core::Result<std::unique_ptr<gp::GameplayScriptHost>> created =
            gp::GameplayScriptHost::Create(config);
        if (!created) {
            const std::string why(created.Err().Message());
            ErrorFmt("FAIL: host create: %s\n", why.c_str());
            return false;
        }
        host = std::move(created).Value();

        // 装配顺序：先注入依赖，再装载（装载期会做契约探针与会话初始化）。
        if (!host->BindEntityManager(entities)) {
            ErrorFmt("FAIL: BindEntityManager\n");
            return false;
        }
        if (!host->BindEventBus(bus)) {
            ErrorFmt("FAIL: BindEventBus\n");
            return false;
        }
        if (!host->BindCommandBus(commands)) {
            ErrorFmt("FAIL: BindCommandBus\n");
            return false;
        }
        host->BindCommandHandler(&Assembly::Handle, this);
        host->SetReferenceRegistry(&references);

        const core::Result<std::size_t> loaded = host->LoadManifestFromFile(kScriptManifest);
        if (!loaded) {
            const std::string why(loaded.Err().Message());
            ErrorFmt("FAIL: LoadManifest: %s\n", why.c_str());
            return false;
        }
        return true;
    }

    /// 脚本受控写的落地端（唯一写入路径）。
    static core::Result<script::ScriptReply> Handle(const script::ScriptCommand& cmd, void* user) {
        return static_cast<Assembly*>(user)->Apply(cmd);
    }

    /// 装配层把「外部事件」路由给脚本层（AI / Inventory / Scene 的真实发布者由 C++ 承担）。
    std::size_t Kill(std::uint32_t monster_id, std::int64_t count = 1) {
        gp::GameplayPayload payload;
        (void)payload.SetName("monster_killed");
        (void)payload.AddInt("monster_id", static_cast<std::int64_t>(monster_id));
        (void)payload.AddInt("count", count);
        (void)payload.AddInt("player", static_cast<std::int64_t>(kPlayer));
        return host->DispatchQuestEvent(payload);
    }

    std::size_t Collect(std::uint32_t item_id, std::int64_t count = 1) {
        gp::GameplayPayload payload;
        (void)payload.SetName("item_collected");
        (void)payload.AddInt("item_id", static_cast<std::int64_t>(item_id));
        (void)payload.AddInt("count", count);
        (void)payload.AddInt("player", static_cast<std::int64_t>(kPlayer));
        return host->DispatchQuestEvent(payload);
    }

    std::size_t NpcTalked(std::uint32_t npc_id) {
        gp::GameplayPayload payload;
        (void)payload.SetName("npc_talked");
        (void)payload.AddInt("npc_id", static_cast<std::int64_t>(npc_id));
        (void)payload.AddInt("player", static_cast<std::int64_t>(kPlayer));
        return host->DispatchQuestEvent(payload);
    }

    std::size_t LocationReached(std::uint32_t zone_id) {
        gp::GameplayPayload payload;
        (void)payload.SetName("location_reached");
        (void)payload.AddInt("zone_id", static_cast<std::int64_t>(zone_id));
        (void)payload.AddInt("player", static_cast<std::int64_t>(kPlayer));
        return host->DispatchQuestEvent(payload);
    }

    /// 驱动 Tick（脚本的周期回调 + EventBus 队列派发）。
    void Advance(double dt_seconds) {
        (void)host->Tick(dt_seconds);
        const core::Result<std::size_t> left = bus.Drain(64, core::DurationMs(20));
        if (!left) {
            ErrorFmt("FAIL: bus.Drain\n");
            ++failures;
        }
    }

    core::Result<script::ScriptReply> Apply(const script::ScriptCommand& cmd) {
        const std::string_view op = cmd.Op();
        script::ScriptReply reply;
        reply.valid = true;

        if (op == "quest.set_progress") {
            if (cmd.args.Size() < 3) {
                return Fail("quest.set_progress needs 3 args");
            }
            const auto player = static_cast<quest::PlayerId>(cmd.args.At(0).AsInt());
            const auto index = cmd.args.At(1).AsInt();
            const auto value = cmd.args.At(2).AsInt();
            if (progress_quest == 0) {
                return Fail("no progress quest in session");
            }
            const quest::QuestDef* def = quests.FindDef(progress_quest);
            if (def == nullptr) {
                return Fail("progress quest def missing");
            }
            if (index < 0 || static_cast<std::size_t>(index) >= def->objectives.size()) {
                return Fail("objective index out of range");
            }
            const quest::ObjectiveDef& objective = def->objectives[static_cast<std::size_t>(index)];
            const auto count = static_cast<std::uint32_t>(value <= 0 ? 1 : value);
            const core::Result<void> written = quests.OnEvent(quest::QuestSystem::Event{
                objective.type, objective.target_id, count, player});
            if (!written) {
                return core::Result<script::ScriptReply>::Fail(written.Err());
            }
            ++progress_writes;
            return core::Result<script::ScriptReply>::Ok(reply);
        }

        if (op == "quest.complete") {
            if (cmd.args.Size() < 2) {
                return Fail("quest.complete needs 2 args");
            }
            const auto player = static_cast<quest::PlayerId>(cmd.args.At(0).AsInt());
            const auto quest_id = static_cast<quest::QuestId>(cmd.args.At(1).AsInt());
            const core::Result<void> turned =
                quests.TurnIn(player, quest_id, core::kInvalidTraceId);
            if (!turned) {
                return core::Result<script::ScriptReply>::Fail(turned.Err());
            }
            ++turn_ins;
            return core::Result<script::ScriptReply>::Ok(reply);
        }

        // 以下两条只做「记账」：本测试不引入 combat / inventory 模块，
        // 但同样断言「脚本只能提交意图，落地的是状态 Owner」。
        if (op == "entity.set_hp") {
            ++hp_writes;
            return core::Result<script::ScriptReply>::Ok(reply);
        }
        if (op == "skill.cast") {
            ++skill_casts;
            return core::Result<script::ScriptReply>::Ok(reply);
        }
        return Fail("unknown op");
    }

    static core::Result<script::ScriptReply> Fail(const char* what) {
        return core::Result<script::ScriptReply>::Fail(
            core::Error(core::ErrorCode::INVALID_ARGUMENT, what, core::domain::kLua));
    }

    core::EventBus bus;
    core::CommandBus commands;
    game::EntityManager entities;
    quest::QuestSystem quests;
    LevelMock levels;
    RewardLog rewards;
    ReferenceMock references;

    std::unique_ptr<gp::GameplayScriptHost> host;

    /// 装配层会话状态：当前「任务事件」服务于哪个 QuestDef（见类注释）。
    quest::QuestId progress_quest{0};
    int progress_writes = 0;
    int turn_ins = 0;
    int hp_writes = 0;
    int skill_casts = 0;
};

// ===========================================================================
// 链路 A：击杀 → 进度 → 完成 → 交还 → 发奖（全链路，含幂等）
// ===========================================================================

void test_chain_kill_to_reward(Assembly& asm_) {
    CHECK(asm_.host->Stats().loaded == 15);
    CHECK(asm_.host->Stats().failed == 0);
    CHECK(asm_.progress_writes == 0);

    const core::Result<void> accepted =
        asm_.quests.Accept(kPlayer, 1001, core::kInvalidTraceId);
    CHECK(accepted.HasValue());
    asm_.progress_quest = 1001;  // 装配层为本次会话指定任务（见 Assembly 注释）

    const quest::QuestInstance* inst = asm_.quests.Find(kPlayer, 1001);
    CHECK(inst != nullptr);
    if (inst == nullptr) {
        return;
    }
    CHECK(inst->status == quest::QuestStatus::Accepted);
    CHECK(inst->progress.size() == 1);

    // ---- 第 1 只怪：Lua 认领事件 → 提交进度意图 → QuestSystem 真正改写 ----
    CHECK(asm_.Kill(1001) == 1);  // 只有 kill_10_wolves 认领 monster_killed
    inst = asm_.quests.Find(kPlayer, 1001);
    CHECK(inst != nullptr);
    if (inst != nullptr) {
        CHECK(inst->progress[0] == 1);
        CHECK(inst->status == quest::QuestStatus::Accepted);
    }
    CHECK(asm_.progress_writes == 1);
    CHECK(asm_.turn_ins == 0);
    CHECK(asm_.rewards.grants == 0);

    // 非狼族怪物（野猪 1003）不认领 → 进度不动（脚本规则在起作用，而不是「什么都算」）
    CHECK(asm_.Kill(1003) == 1);  // 事件仍被脚本收到，但脚本内部判家族后不动进度
    inst = asm_.quests.Find(kPlayer, 1001);
    CHECK(inst != nullptr);
    if (inst != nullptr) {
        CHECK(inst->progress[0] == 1);
    }

    // ---- 第 2、3 只：达标 → QuestSystem 发布 QuestCompleted → 装配层转发 → Lua 交还 ----
    CHECK(asm_.Kill(1001) == 1);
    inst = asm_.quests.Find(kPlayer, 1001);
    CHECK(inst != nullptr);
    if (inst != nullptr) {
        CHECK(inst->progress[0] == 2);
    }

    CHECK(asm_.Kill(1001) == 1);
    asm_.Advance(0.1);  // 派发 QuestCompleted（EventBus 入队 → Drain）

    inst = asm_.quests.Find(kPlayer, 1001);
    CHECK(inst != nullptr);
    if (inst != nullptr) {
        CHECK(inst->progress[0] == 3);
        CHECK(inst->status == quest::QuestStatus::TurnedIn);
    }
    // 奖励由 QuestSystem 经 IRewardSink 发放 —— 脚本看不到也改不了数值（§7）
    CHECK(asm_.rewards.grants == 1);
    CHECK(asm_.rewards.last_player == kPlayer);
    CHECK(asm_.rewards.last_quest == 1001);
    CHECK(asm_.rewards.last_exp == 120);
    CHECK(asm_.rewards.last_currency == 50);
    CHECK(asm_.turn_ins == 1);

    // ---- 幂等：再发一次 QuestCompleted，Lua 的 `turned_in` 与 QuestSystem 的幂等表双重拦住 ----
    asm_.Advance(0.1);
    CHECK(asm_.rewards.grants == 1);
    CHECK(asm_.turn_ins == 1);

    // 击杀事件在任务已交还后继续到达：进度不再推进（QuestSystem 只推进 Accepted 的任务）
    CHECK(asm_.Kill(1001) == 1);
    inst = asm_.quests.Find(kPlayer, 1001);
    CHECK(inst != nullptr);
    if (inst != nullptr) {
        CHECK(inst->progress[0] == 3);
        CHECK(inst->status == quest::QuestStatus::TurnedIn);
    }
}

// ===========================================================================
// 链路 B：采集中途热更 → 进度不被重置
// ===========================================================================

void test_chain_collect_with_hot_reload(Assembly& asm_) {
    const core::Result<void> accepted =
        asm_.quests.Accept(kPlayer, 1002, core::kInvalidTraceId);
    CHECK(accepted.HasValue());  // 前置 1001 已交还 + 等级 10
    asm_.progress_quest = 1002;

    // 采集 2 个普通草药（脚本侧权重 1）
    CHECK(asm_.Collect(9001, 2) == 1);
    const quest::QuestInstance* inst = asm_.quests.Find(kPlayer, 1002);
    CHECK(inst != nullptr);
    if (inst != nullptr) {
        CHECK(inst->progress[0] == 2);
        CHECK(inst->status == quest::QuestStatus::Accepted);
    }

    // ---- 任务进行到一半时热更 collect_herbs（六阶段流水线：Prepare → Validate → Activate）----
    const std::string source = ReadFile(kCollectHerbsPath);
    CHECK(!source.empty());
    if (source.empty()) {
        return;
    }
    const core::Result<script::ReloadTicket> ticket =
        asm_.host->PrepareReload("quest/collect_herbs", source);
    CHECK(ticket.HasValue());
    if (ticket) {
        CHECK(asm_.host->ValidateReload(ticket.Value()).HasValue());
        asm_.host->BeginSafePoint(1);  // 宿主在 Tick 边界开安全点
        CHECK(asm_.host->ActivateReload(ticket.Value(), core::kInvalidTraceId).HasValue());
        asm_.host->EndSafePoint();
    }

    // ---- 热更后继续采：脚本仍在线，且 QuestSystem 里的进度**没有被重置** ----
    CHECK(asm_.Collect(9001, 3) == 1);
    inst = asm_.quests.Find(kPlayer, 1002);
    CHECK(inst != nullptr);
    if (inst != nullptr) {
        CHECK(inst->progress[0] == 5);  // 2 + 3，累计而不是从 0 重来
        CHECK(inst->status == quest::QuestStatus::Completed);
    }
    CHECK(asm_.rewards.grants == 1);  // collect_herbs 不处理 quest_completed；交还仍走 QuestSystem

    // 未识别的物品不推进（脚本里的品质白名单在起作用）
    CHECK(asm_.Collect(9002, 1) == 1);
    inst = asm_.quests.Find(kPlayer, 1002);
    CHECK(inst != nullptr);
    if (inst != nullptr) {
        CHECK(inst->progress[0] == 5);
    }
}

// ===========================================================================
// 链路 C：限时护送（on_tick 倒计时 + 超时自动失效）
// ===========================================================================

void test_chain_timed_escort(Assembly& asm_) {
    // 前置：任务 1003（对话）是 1004 的前置，先走 QuestSystem 自身路径完成并交还。
    CHECK(asm_.quests.Accept(kPlayer, 1003, core::kInvalidTraceId).HasValue());
    CHECK(asm_.quests
              .OnEvent(quest::QuestSystem::Event{quest::ObjectiveType::TalkNpc, 2002, 1, kPlayer})
              .HasValue());
    asm_.Advance(0.1);  // 派发 1003 的 QuestCompleted（脚本不认领，属预期）
    CHECK(asm_.quests.TurnIn(kPlayer, 1003, core::kInvalidTraceId).HasValue());
    CHECK(asm_.rewards.grants == 2);
    CHECK(asm_.rewards.last_quest == 1003);

    CHECK(asm_.quests.Accept(kPlayer, 1004, core::kInvalidTraceId).HasValue());
    asm_.progress_quest = 1004;

    // ---- 与 NPC 对话 → 脚本启动 60 秒倒计时（Lua 侧规则，时间由宿主 dt 注入）----
    const std::uint64_t ticks_before = asm_.host->TickNumber();
    CHECK(asm_.NpcTalked(2002) == 1);
    asm_.Advance(1.0);
    CHECK(asm_.host->TickNumber() == ticks_before + 1);  // 倒计时由 on_tick 推进一次

    // ---- 到达终点 → 脚本提交进度 + 请求交还 → QuestSystem 真正完成并发奖 ----
    CHECK(asm_.LocationReached(3001) == 1);
    asm_.Advance(0.1);
    const quest::QuestInstance* inst = asm_.quests.Find(kPlayer, 1004);
    CHECK(inst != nullptr);
    if (inst != nullptr) {
        CHECK(inst->progress[0] == 1);
        CHECK(inst->status == quest::QuestStatus::TurnedIn);
    }
    CHECK(asm_.rewards.grants == 3);
    CHECK(asm_.rewards.last_quest == 1004);
    CHECK(asm_.rewards.last_exp == 150);

    // ---- 再护送一次并让它超时：脚本拒绝提交任何进度（限时任务超时即失效）----
    CHECK(asm_.NpcTalked(2002) == 1);
    asm_.Advance(61.0);  // 一次长 dt 直接吃掉 60 秒预算（on_tick 判定超时）
    CHECK(asm_.LocationReached(3001) == 1);  // 事件到达，但脚本已判定 expired
    asm_.Advance(0.1);
    CHECK(asm_.rewards.grants == 3);  // 没有第二次发奖
    inst = asm_.quests.Find(kPlayer, 1004);
    CHECK(inst != nullptr);
    if (inst != nullptr) {
        CHECK(inst->status == quest::QuestStatus::TurnedIn);  // 状态保持终态，未被改写
    }
}

// ===========================================================================
// 链路 D：Lua 算伤害 → C++ 结算 → 血量跨阈值 → Lua 切阶段
// ===========================================================================

void test_chain_skill_damage_to_boss_phase(Assembly& asm_) {
    gp::SkillFormulaRequest request;
    request.skill_id = "fireball";
    request.caster = kPlayer;
    request.target = 9001;
    request.base = 40.0;
    request.coefficient = 1.35;
    request.attack_power = 100.0;
    request.target_hp_pct = 100.0;
    request.caster_level = 10;

    // 公式由 Lua 算（§7：脚本只给数值）
    const gp::SkillFormulaResult formula = asm_.host->ComputeSkillFormula(request);
    CHECK(formula.matched);
    CHECK(formula.ok);
    CHECK(formula.script == "skill/fireball");
    const double damage = formula.amount;

    // 结算（暴击 / 减免 / 扣血）由 C++ 完成：这里用「状态 Owner 视角」算血量百分比
    double boss_hp = kDragonMaxHp - damage;
    double hp_pct = boss_hp / kDragonMaxHp * 100.0;
    CHECK(hp_pct <= 70.0);

    // 阶段 1：跨过 70% → 2 阶段 + 换技能组 + 要求广播
    gp::BossContext ctx;
    ctx.boss_template = "dragon";
    ctx.boss = 9001;
    ctx.hp_pct = hp_pct;
    ctx.phase = 1;
    const gp::BossPhaseDecision phase2 = asm_.host->CheckBossPhase(ctx);
    CHECK(phase2.matched);
    CHECK(phase2.ok);
    CHECK(phase2.switched);
    CHECK(phase2.phase == 2);
    CHECK(phase2.skill_group == 1);
    CHECK(phase2.broadcast);       // 广播的**内容**由 C++ 组装，脚本只说「要广播」
    CHECK(phase2.summon_npc == 0);
    CHECK(asm_.host->ScriptErrorCount() == 0);

    // 阶段 2：再吃一发 → 跨过 40% → 3 阶段 + 召唤
    boss_hp -= damage;
    hp_pct = boss_hp / kDragonMaxHp * 100.0;
    CHECK(hp_pct <= 40.0);
    ctx.hp_pct = hp_pct;
    ctx.phase = 2;
    const gp::BossPhaseDecision phase3 = asm_.host->CheckBossPhase(ctx);
    CHECK(phase3.ok);
    CHECK(phase3.switched);
    CHECK(phase3.phase == 3);
    CHECK(phase3.skill_group == 2);
    CHECK(phase3.summon_npc == 3001);

    // 同阶段重复查询 → 脚本不回传结果 → C++ 不会重复广播（防广播风暴）
    ctx.phase = 3;
    const gp::BossPhaseDecision repeat = asm_.host->CheckBossPhase(ctx);
    CHECK(repeat.matched);
    CHECK(!repeat.ok);
    CHECK(!repeat.switched);
}

// ===========================================================================
// 链路 E：热更实战演练（生效 + 回滚），且不影响其它链路
// ===========================================================================

void test_chain_hot_reload_drill(Assembly& asm_) {
    const std::string original = ReadFile(kFireballPath);
    CHECK(!original.empty());
    if (original.empty()) {
        return;
    }

    auto damage_of_fireball = [&asm_]() {
        gp::SkillFormulaRequest request;
        request.skill_id = "fireball";
        request.attack_power = 100.0;
        request.target_hp_pct = 100.0;
        return asm_.host->ComputeSkillFormula(request).amount;
    };

    CHECK(damage_of_fireball() == 175.0);  // 40 + 1.35 × 100

    std::string patched = original;
    CHECK(ReplaceOnce(patched, "local BASE = 40.0", "local BASE = 100.0"));

    const core::Result<script::ReloadTicket> ticket =
        asm_.host->PrepareReload("skill/fireball", patched);
    CHECK(ticket.HasValue());
    if (ticket) {
        const core::Result<script::ValidationReport> report =
            asm_.host->ValidateReload(ticket.Value());
        CHECK(report.HasValue());
        if (report) {
            CHECK(report.Value().ok);
        }
        asm_.host->BeginSafePoint(2);
        CHECK(asm_.host->ActivateReload(ticket.Value(), core::kInvalidTraceId).HasValue());
        asm_.host->EndSafePoint();
    }
    // 线上即时生效（无需重启、无需重载其余 14 个脚本）
    CHECK(damage_of_fireball() == 235.0);  // 100 + 1.35 × 100
    CHECK(asm_.host->Stats().loaded == 15);

    // 回滚 → 旧公式恢复
    asm_.host->BeginSafePoint(3);
    CHECK(asm_.host->Rollback("skill/fireball", core::kInvalidTraceId).HasValue());
    asm_.host->EndSafePoint();
    CHECK(damage_of_fireball() == 175.0);

    // 其它脚本完全没有受影响（只换掉目标脚本的模块表）
    CHECK(asm_.host->ComputeSkillFormula(gp::SkillFormulaRequest{
              "ice_lance", 0, 0, 0.0, 0.0, 100.0, 0.0, 100.0, 1, 0})
              .amount == 130.0);
}

}  // namespace

int main() {
    Line("== TASK-033 gameplay_integration_test ==\n");

    Assembly assembly;
    if (!assembly.Setup()) {
        ErrorFmt("TASK-033 integration test SETUP FAILED\n");
        return 1;
    }
    LineFmt("setup: scripts=%zu quests=%zu reward_sink=ok\n",
            assembly.host->Stats().loaded, assembly.quests.DefCount());

    test_chain_kill_to_reward(assembly);
    test_chain_collect_with_hot_reload(assembly);
    test_chain_timed_escort(assembly);
    test_chain_skill_damage_to_boss_phase(assembly);
    test_chain_hot_reload_drill(assembly);

    // 全链路跑完后：没有任何脚本错误被隔离（§19 的「不该报错的别报错」）
    CHECK(assembly.host->ScriptErrorCount() == 0);
    CHECK(assembly.host->Stats().loaded == 15);
    LineFmt("totals: progress_writes=%d turn_ins=%d rewards=%zu hp_writes=%d skill_casts=%d\n",
            assembly.progress_writes, assembly.turn_ins, assembly.rewards.grants,
            assembly.hp_writes, assembly.skill_casts);

    LineFmt("checks=%d failures=%d\n", checks, failures);
    if (failures > 0) {
        ErrorFmt("TASK-033 integration test FAILED: %d/%d\n", failures, checks);
        return 1;
    }
    Line("TASK-033 integration test OK\n");
    return 0;
}
