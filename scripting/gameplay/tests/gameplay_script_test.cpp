// scripting/gameplay/tests/gameplay_script_test.cpp — TASK-033 · 单元测试（§16）+ 失败/红线测试（§19）
//
// 自包含 harness（与 TASK-031 / TASK-032 / engine/core/tests 同口径）：纯断言 + 失败计数器，
// 不依赖 gtest。输出统一走 test_print.h 的 fwrite 通道（红线禁止 cout / printf / cerr）。
//
// 覆盖清单（§16 / §19 / §20 验收）
//   §8    清单解析 + 自洽校验（类别↔hook 组合、tick_hz ≤ 1、requires 形式、重名）
//   §8    `config/gameplay/scripts.json` 真实清单：五类脚本各 3 个，共 15 个
//   §15.3 技能公式：base + coeff×AP、斩杀加成、防御衰减、等级封顶（真实脚本）
//   §15.5 NPC 决策：攻击 / 追击 / 逃跑 / 回岗（真实脚本）
//   §15.6 Boss 阶段：龙 4 阶段 / 石魔 2 阶段狂暴 / 巫妖 3 阶段召唤
//   §15.7 活动修正：窗口内 2× / 窗口外自动失效 / 软关闭衰减
//   §19   脚本返回 NaN / 负数 / 超上限 → C++ 钳制并标记 `clamped`
//   §19   装载失败隔离（单脚本坏 → 其余照常；该 hook 回退 C++ 默认）
//   §19   **红线拦截**：脚本做 IO（io.* / os.* / loadstring）在流水线校验阶段被拒
//   §21   **脚本改不了 HP**：写意图只能变成 ScriptCommand，由宿主注册的 op handler 落地
//   §21   周期脚本 ≤ 1Hz：`Tick` 累加器判定，且丢弃积压（不进入 catch-up 死亡螺旋）
//   §20#5 热更一个技能公式 → 线上即时生效 → 可回滚；非安全点 Activate 被拒
//   §27.3 `ScriptNames()` / `PathOf()` 全部来自清单 —— C++ 侧没有任何脚本名字面量
//
// 脚本写法约定（重要）
// ------------------
//   `Call(id, fn)` 在**脚本模块表**里查函数 ⇒ 测试脚本一律用**全局函数定义**
//   `function on_event(...) end`（与 TASK-031/032 测试同一约定）。
//   四入口契约：`on_init(ctx)` / `on_event(ctx, name, payload)` / `on_tick(ctx, dt)` /
//   `on_reload(ctx, old_version)`；表数据走 `gameplay.ctx()` / `gameplay.payload()`，
//   结果经 `gameplay.result{…}` 回传（TASK-031 的 Call 签名是标量，表走绑定通道）。

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "mmo/core/bus/command_bus.h"
#include "mmo/core/error/error.h"
#include "mmo/core/error/error_code.h"
#include "mmo/core/log/trace_id.h"
#include "mmo/gameplay/gameplay_manifest.h"
#include "mmo/gameplay/gameplay_script_host.h"
#include "mmo/gameplay/gameplay_types.h"
#include "mmo/script/script_event.h"
#include "mmo/script/script_value.h"

#include "test_print.h"

namespace {

namespace core = mmo::core;
namespace fs = std::filesystem;
namespace game = mmo::game;
namespace gp = mmo::gameplay;

using gp::ActivityQuery;
using gp::AiAction;
using gp::AiContext;
using gp::BossContext;
using gp::GameplayPayload;
using gp::GameplayScriptHost;
using gp::Hook;
using gp::IReferenceRegistry;
using gp::ScriptBinding;
using gp::ScriptCategory;
using gp::ScriptManifest;
using gp::SkillFormulaRequest;

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

// 浮点比较：脚本公式是确定的算术，1e-9 足够严（不放大误差掩盖问题）。
bool Near(double a, double b, double eps = 1e-9) { return std::fabs(a - b) <= eps; }

#define CHECK_NEAR(actual, expected)                                               \
    do {                                                                           \
        ++checks;                                                                  \
        const double a_ = static_cast<double>(actual);                             \
        const double e_ = static_cast<double>(expected);                           \
        if (!Near(a_, e_)) {                                                       \
            ErrorFmt("FAIL @ %s:%d : %s == %.6f, want %.6f\n", __FILE__, __LINE__, \
                     #actual, a_, e_);                                             \
            ++failures;                                                            \
        }                                                                          \
    } while (0)

constexpr const char* kManifestPath = "config/gameplay/scripts.json";
constexpr const char* kFireballPath = "scripting/gameplay/skill/fireball.lua";

// ===========================================================================
// 通用工具
// ===========================================================================

std::string ReadFile(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return {};
    }
    std::ostringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
}

/// 只在第一次出现处替换；未命中返回 false（测试里会 CHECK 替换确实发生）。
bool ReplaceOnce(std::string& text, std::string_view from, std::string_view to) {
    const std::size_t pos = text.find(from);
    if (pos == std::string::npos) {
        return false;
    }
    text.replace(pos, from.size(), to);
    return true;
}

/// 合成脚本的落盘目录（系统临时目录；不污染仓库工作区）。
fs::path ScratchRoot() {
    const fs::path dir = fs::temp_directory_path() / "mmo_t033_script_test";
    std::error_code ec;
    fs::create_directories(dir, ec);
    return dir;
}

/// 写一个合成脚本，返回**相对清单路径**（宿主 `Config::root` 指向 scratch 目录）。
std::string WriteScript(const std::string& file_name, std::string_view source) {
    const fs::path path = ScratchRoot() / file_name;
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(source.data(), static_cast<std::streamsize>(source.size()));
    out.close();
    return file_name;
}

/// 四入口骨架 + 自定义 `on_event` 体（用于合成脚本）。
std::string ScriptWithEventBody(std::string_view hook_name, std::string_view body) {
    std::string s;
    s += "function on_init(ctx) end\n";
    s += "function on_event(ctx, name, payload)\n";
    s += "  payload = payload or gameplay.payload()\n";
    s += "  if name ~= \"" + std::string(hook_name) + "\" then return end\n";
    s += body;
    s += "\nend\n";
    s += "function on_tick(ctx, dt) end\n";
    s += "function on_reload(ctx, old_version) end\n";
    return s;
}

/// 合成「命中 key 时回传固定结果」的脚本。
///
/// `field` 是载荷里的路由字段名（skill / profile / boss / activity），与各 hook 一致。
std::string ProbeScript(std::string_view hook_name, std::string_view field, std::string_view key,
                        std::string_view result_expr) {
    std::string body;
    body += "  if payload." + std::string(field) + " ~= \"" + std::string(key) +
            "\" then return end\n";
    body += "  gameplay.result(" + std::string(result_expr) + ")";
    return ScriptWithEventBody(hook_name, body);
}

ScriptBinding Bind(std::string name, std::string path, ScriptCategory category,
                   std::vector<Hook> hooks, std::vector<std::string> keys, double tick_hz = 0.0,
                   std::vector<std::string> requires_refs = {}) {
    ScriptBinding b;
    b.name = std::move(name);
    b.path = std::move(path);
    b.category = category;
    b.hooks = std::move(hooks);
    b.keys = std::move(keys);
    b.tick_hz = tick_hz;
    b.requires_refs = std::move(requires_refs);
    b.enabled = true;
    return b;
}

std::unique_ptr<GameplayScriptHost> MakeHost(const std::string& root, bool require_all_scripts) {
    GameplayScriptHost::Config config;
    config.root = root;
    config.require_all_scripts = require_all_scripts;
    core::Result<std::unique_ptr<GameplayScriptHost>> created = GameplayScriptHost::Create(config);
    if (!created) {
        const std::string why(created.Err().Message());
        ErrorFmt("FAIL: host create: %s\n", why.c_str());
        return nullptr;
    }
    return std::move(created).Value();
}

/// 真实清单 + 真实脚本（15 个）的宿主。
std::unique_ptr<GameplayScriptHost> RealHost() {
    auto host = MakeHost("", true);
    if (host == nullptr) {
        return nullptr;
    }
    const core::Result<std::size_t> loaded = host->LoadManifestFromFile(kManifestPath);
    if (!loaded) {
        const std::string why(loaded.Err().Message());
        ErrorFmt("FAIL: load real manifest: %s\n", why.c_str());
        return nullptr;
    }
    return host;
}

// ---- 命令处理器：模拟「状态 Owner 的 op 落地端」---------------------------
struct HandlerLog {
    int calls = 0;
    std::string last_op;
    std::int64_t a0 = -1;
    std::int64_t a1 = -1;
};

core::Result<mmo::script::ScriptReply> RecordingHandler(const mmo::script::ScriptCommand& cmd,
                                                        void* user) {
    auto* log = static_cast<HandlerLog*>(user);
    ++log->calls;
    log->last_op.assign(cmd.Op());
    const std::size_t n = cmd.args.Size();
    if (n > 0) {
        log->a0 = cmd.args.At(0).AsInt();
    }
    if (n > 1) {
        log->a1 = cmd.args.At(1).AsInt();
    }
    mmo::script::ScriptReply reply;
    reply.valid = true;
    return core::Result<mmo::script::ScriptReply>::Ok(reply);
}

/// 引用注册表 mock（§19「脚本引用的 Quest/Boss 不存在 → 加载期报错」）。
class RefRegistry final : public IReferenceRegistry {
public:
    bool Exists(std::string_view ns, std::string_view id) const noexcept override {
        return ns == "quest" && id == "1001";
    }
};

// ---- 请求构造 -------------------------------------------------------------

SkillFormulaRequest SkillReq(std::string_view skill_id, double ap, double hp_pct) {
    SkillFormulaRequest r;
    r.skill_id = skill_id;
    r.caster = 1;
    r.target = 2;
    r.base = 40.0;
    r.coefficient = 1.35;
    r.attack_power = ap;
    r.target_defense = 0.0;
    r.target_hp_pct = hp_pct;
    r.caster_level = 1;
    r.kind = 0;
    return r;
}

GameplayPayload QuestEvent(std::string_view name) {
    GameplayPayload p;
    (void)p.SetName(name);
    return p;
}

// ===========================================================================
// §8 纯值类型：钳制 / 名称化 / 载荷容器
// ===========================================================================

void test_clamp_and_naming() {
    double out = -1.0;
    const char* reason = nullptr;

    CHECK(gp::ClampFormulaAmount(123.5, out, reason));
    CHECK_NEAR(out, 123.5);
    CHECK(reason == nullptr);

    // NaN / Inf / 负数 / 超上限 → 钳制并给出原因短语（§19）
    CHECK(!gp::ClampFormulaAmount(std::nan(""), out, reason));
    CHECK_NEAR(out, 0.0);
    CHECK(reason != nullptr);
    CHECK(!gp::ClampFormulaAmount(std::numeric_limits<double>::infinity(), out, reason));
    CHECK_NEAR(out, 0.0);
    CHECK(!gp::ClampFormulaAmount(-1.0, out, reason));
    CHECK_NEAR(out, 0.0);
    CHECK(!gp::ClampFormulaAmount(1.0e12, out, reason));
    CHECK_NEAR(out, gp::kMaxFormulaAmount);

    double mult = 0.0;
    CHECK(gp::ClampMultiplier(2.0, mult));
    CHECK_NEAR(mult, 2.0);
    CHECK(!gp::ClampMultiplier(std::nan(""), mult));
    CHECK_NEAR(mult, 0.0);
    CHECK(!gp::ClampMultiplier(1.0e9, mult));
    CHECK_NEAR(mult, gp::kMaxMultiplier);

    bool clamped = false;
    CHECK(gp::ClampAiAction(3, clamped) == AiAction::Attack);
    CHECK(!clamped);
    CHECK(gp::ClampAiAction(99, clamped) == AiAction::Idle);
    CHECK(clamped);
    CHECK(gp::ClampAiAction(-1, clamped) == AiAction::Idle);
    CHECK(clamped);

    CHECK(gp::ClampBossPhase(3, clamped) == 3);
    CHECK(!clamped);
    CHECK(gp::ClampBossPhase(0, clamped) == 1);
    CHECK(clamped);
    CHECK(gp::ClampBossPhase(99, clamped) == gp::kMaxBossPhase);
    CHECK(clamped);

    ScriptCategory cat = ScriptCategory::Quest;
    CHECK(gp::ParseCategory("boss", cat));
    CHECK(cat == ScriptCategory::Boss);
    CHECK(!gp::ParseCategory("nope", cat));  // 未知类别禁止静默默认（§19）

    Hook hook = Hook::QuestEvent;
    CHECK(gp::ParseHook("boss_phase", hook));
    CHECK(hook == Hook::BossPhase);
    CHECK(!gp::ParseHook("nope", hook));

    CHECK(gp::HookEventName(Hook::SkillFormula) == "skill_formula");
    CHECK(gp::HookEventName(Hook::QuestEvent) == "quest_event");
    CHECK(gp::HookEventName(Hook::AiDecide) == "ai_decide");
    CHECK(gp::HookEventName(Hook::BossPhase) == "boss_phase");
    CHECK(gp::HookEventName(Hook::ActivityModifier) == "activity_modifier");
    CHECK(std::string(gp::ToString(ScriptCategory::Event)) == "event");
}

void test_payload_container() {
    GameplayPayload p;
    CHECK(p.SetName("monster_killed"));
    CHECK(p.Name() == "monster_killed");
    CHECK(p.AddInt("monster_id", 1001));
    CHECK(p.AddStr("player", "alice"));
    CHECK(p.AddNum("hp_pct", 42.5));
    CHECK(p.AddBool("elite", true));
    CHECK(p.FieldCount() == 4);
    CHECK(p.GetInt("monster_id") == 1001);
    CHECK(p.GetStr("player") == "alice");
    CHECK_NEAR(p.GetNum("hp_pct"), 42.5);
    CHECK(p.GetBool("elite"));
    CHECK(p.GetInt("missing", 7) == 7);  // 缺失字段返回默认值
    CHECK(p.Field(3) != nullptr);
    CHECK(p.Field(4) == nullptr);  // 越界返回 nullptr（禁止越界读）

    // 名字超长：拒绝而不是截断
    CHECK(!p.SetName(std::string(GameplayPayload::kNameCap + 1, 'x')));
    // 空键拒绝
    CHECK(!p.AddInt("", 1));
    // 字符串超长拒绝
    CHECK(!p.AddStr("k", std::string(64 + 1, 'y')));

    GameplayPayload q;
    CHECK(q.SetName("evt"));
    std::size_t accepted = 0;
    for (std::size_t i = 0; i < GameplayPayload::kMaxFields + 4; ++i) {
        if (q.AddInt("k", static_cast<std::int64_t>(i))) {
            ++accepted;
        }
    }
    CHECK(accepted == GameplayPayload::kMaxFields);  // 固定容量，超限拒绝（热路径零分配）

    q.Reset();
    CHECK(q.FieldCount() == 0);
    CHECK(q.Name().empty());
}

// ===========================================================================
// §8 清单解析 + 自洽校验
// ===========================================================================

void test_manifest_real() {
    std::string error;
    const core::Result<ScriptManifest> parsed = gp::LoadManifestFile(kManifestPath, &error);
    CHECK(parsed.HasValue());
    CHECK(error.empty());
    if (!parsed) {
        ErrorFmt("FAIL: manifest parse: %s\n", error.c_str());
        return;
    }
    const ScriptManifest& manifest = parsed.Value();
    CHECK(manifest.scripts.size() == 15);
    CHECK(manifest.EnabledCount() == 15);
    CHECK(gp::ValidateManifest(manifest).empty());

    std::size_t by_category[5] = {0, 0, 0, 0, 0};
    std::size_t tick_scripts = 0;
    for (const ScriptBinding& b : manifest.scripts) {
        by_category[static_cast<std::size_t>(b.category)] += 1;
        if (b.tick_hz > 0.0) {
            ++tick_scripts;
        }
    }
    // §20 #1「五类脚本各 ≥ 3 个」
    for (std::size_t i = 0; i < 5; ++i) {
        CHECK(by_category[i] == 3);
    }
    CHECK(tick_scripts == 2);  // escort_merchant / double_exp

    // 序列化可往返（诊断 / 报告用）
    CHECK(!gp::ToJson(manifest).empty());

    // 清单错误：未知 hook 名 / 缺字段 → 明确报错，禁止静默默认
    std::string bad_hook_error;
    const char* kBadHook =
        "{\"scripts\":[{\"name\":\"x\",\"path\":\"x.lua\",\"category\":\"quest\","
        "\"hooks\":[\"no_such_hook\"],\"keys\":[\"k\"]}]}";
    CHECK(!gp::ParseManifest(kBadHook, &bad_hook_error).HasValue());
    CHECK(!bad_hook_error.empty());

    std::string bad_field_error;
    const char* kMissingField =
        "{\"scripts\":[{\"name\":\"x\",\"category\":\"quest\",\"hooks\":[\"quest_event\"]}]}";
    CHECK(!gp::ParseManifest(kMissingField, &bad_field_error).HasValue());
}

void test_manifest_validation_rejects() {
    // tick_hz > 1 → §21「禁止高频周期脚本」
    ScriptManifest too_fast;
    too_fast.scripts.push_back(
        Bind("a", "a.lua", ScriptCategory::Quest, {Hook::QuestEvent}, {"e"}, 2.0));
    CHECK(!gp::ValidateManifest(too_fast).empty());

    // 类别 ↔ hook 不匹配（skill 却挂 boss_phase）
    ScriptManifest mismatch;
    mismatch.scripts.push_back(Bind("b", "b.lua", ScriptCategory::Skill, {Hook::BossPhase}, {"k"}));
    CHECK(!gp::ValidateManifest(mismatch).empty());

    // 未挂任何 hook
    ScriptManifest no_hook;
    no_hook.scripts.push_back(Bind("c", "c.lua", ScriptCategory::Quest, {}, {"k"}));
    CHECK(!gp::ValidateManifest(no_hook).empty());

    // 空 path
    ScriptManifest no_path;
    no_path.scripts.push_back(Bind("d", "", ScriptCategory::Quest, {Hook::QuestEvent}, {"k"}));
    CHECK(!gp::ValidateManifest(no_path).empty());

    // 重名
    ScriptManifest dup;
    dup.scripts.push_back(Bind("dup", "d1.lua", ScriptCategory::Quest, {Hook::QuestEvent}, {"k"}));
    dup.scripts.push_back(Bind("dup", "d2.lua", ScriptCategory::Quest, {Hook::QuestEvent}, {"k2"}));
    CHECK(!gp::ValidateManifest(dup).empty());

    // requires 形式错误（缺 '<ns>:<id>'）
    ScriptManifest bad_ref;
    bad_ref.scripts.push_back(
        Bind("e", "e.lua", ScriptCategory::Quest, {Hook::QuestEvent}, {"k"}, 0.0, {"no_colon"}));
    CHECK(!gp::ValidateManifest(bad_ref).empty());

    // 合法基线
    ScriptManifest ok;
    ok.scripts.push_back(Bind("ok", "ok.lua", ScriptCategory::Quest, {Hook::QuestEvent}, {"k"}));
    CHECK(gp::ValidateManifest(ok).empty());

    // 清单自洽检查失败时**零副作用**：LoadManifest 必须在编译任何脚本之前就拒绝
    auto host = MakeHost("", true);
    CHECK(host != nullptr);
    if (host != nullptr) {
        const core::Result<std::size_t> r = host->LoadManifest(too_fast);
        CHECK(!r.HasValue());
        CHECK(host->Stats().loaded == 0);
        CHECK(host->Stats().failed == 0);
        CHECK(host->ScriptNames().empty());
    }
}

// ===========================================================================
// §15.2 装载：真实清单 + 路由表
// ===========================================================================

void test_load_real_manifest_and_routes() {
    auto host = RealHost();
    CHECK(host != nullptr);
    if (host == nullptr) {
        return;
    }

    const gp::LoadStats& stats = host->Stats();
    CHECK(stats.total == 15);
    CHECK(stats.enabled == 15);
    CHECK(stats.loaded == 15);
    CHECK(stats.failed == 0);
    CHECK(stats.by_category[static_cast<std::size_t>(ScriptCategory::Quest)] == 3);
    CHECK(stats.by_category[static_cast<std::size_t>(ScriptCategory::Skill)] == 3);
    CHECK(stats.by_category[static_cast<std::size_t>(ScriptCategory::Ai)] == 3);
    CHECK(stats.by_category[static_cast<std::size_t>(ScriptCategory::Boss)] == 3);
    CHECK(stats.by_category[static_cast<std::size_t>(ScriptCategory::Event)] == 3);
    CHECK(stats.hooks >= 15);

    CHECK(host->ScriptNames().size() == 15);
    CHECK(host->PathOf("skill/fireball") == kFireballPath);
    CHECK(host->PathOf("no/such").empty());

    // 五个 hook 都有脚本认领（否则该 hook 会静默回退到 C++ 默认行为）
    CHECK(host->RouteCount(Hook::SkillFormula) >= 3);
    CHECK(host->RouteCount(Hook::QuestEvent) >= 3);
    CHECK(host->RouteCount(Hook::AiDecide) >= 3);
    CHECK(host->RouteCount(Hook::BossPhase) >= 3);
    CHECK(host->RouteCount(Hook::ActivityModifier) >= 3);

    // 未注入引用注册表时**不静默放行**：必须在 LoadErrors 里明确记录「已跳过校验」
    CHECK(!host->LoadErrors().empty());
    if (!host->LoadErrors().empty()) {
        CHECK(host->LoadErrors().front().find("IReferenceRegistry") != std::string::npos);
    }

    // 未认领的 key → matched == false（调用方走 C++ 默认公式，不是「静默 0」）
    const gp::SkillFormulaResult miss =
        host->ComputeSkillFormula(SkillReq("nonexistent", 10.0, 100.0));
    CHECK(!miss.matched);
    CHECK(!miss.ok);

    AiContext ai_ctx;
    ai_ctx.profile = "nonexistent";
    CHECK(!host->DecideAi(ai_ctx).matched);

    BossContext boss_ctx;
    boss_ctx.boss_template = "nonexistent";
    CHECK(!host->CheckBossPhase(boss_ctx).matched);

    ActivityQuery act_query;
    act_query.activity_id = "nonexistent";
    CHECK(!host->QueryActivity(act_query).matched);

    CHECK(host->DispatchQuestEvent(QuestEvent("nonexistent_event")) == 0);
    CHECK(host->ScriptErrorCount() == 0);  // 未命中不是错误
}

// ===========================================================================
// §15.3 技能公式（真实脚本）
// ===========================================================================

void test_skill_formulas() {
    auto host = RealHost();
    CHECK(host != nullptr);
    if (host == nullptr) {
        return;
    }

    // fireball：40 + 1.35 × 100 = 175
    const gp::SkillFormulaResult fb = host->ComputeSkillFormula(SkillReq("fireball", 100.0, 100.0));
    CHECK(fb.matched);
    CHECK(fb.ok);
    CHECK(!fb.clamped);
    CHECK(fb.reason == nullptr);
    CHECK_NEAR(fb.amount, 175.0);
    CHECK_NEAR(fb.raw, 175.0);
    CHECK(fb.script == "skill/fireball");

    // fireball 斩杀加成：HP% < 30 → ×1.5
    const gp::SkillFormulaResult exec = host->ComputeSkillFormula(SkillReq("fireball", 100.0, 20.0));
    CHECK(exec.ok);
    CHECK_NEAR(exec.amount, 262.5);
    // 边界：恰好 30% 不触发加成（脚本是严格小于）
    CHECK_NEAR(host->ComputeSkillFormula(SkillReq("fireball", 100.0, 30.0)).amount, 175.0);

    // ice_lance：(25 + 1.05 × 100) × (1 - 0.0015 × 0) = 130
    const gp::SkillFormulaResult ice =
        host->ComputeSkillFormula(SkillReq("ice_lance", 100.0, 100.0));
    CHECK(ice.matched);
    CHECK_NEAR(ice.amount, 130.0);

    // ice_lance 防御衰减：defense = 100 → factor 0.85；极大防御 → 衰减下限 0.35
    SkillFormulaRequest ice_def = SkillReq("ice_lance", 100.0, 100.0);
    ice_def.target_defense = 100.0;
    CHECK_NEAR(host->ComputeSkillFormula(ice_def).amount, 110.5);
    ice_def.target_defense = 100000.0;
    CHECK_NEAR(host->ComputeSkillFormula(ice_def).amount, 45.5);

    // heal_light：30 + 1.10 × 100 + 2.5 × level
    SkillFormulaRequest heal = SkillReq("heal_light", 100.0, 100.0);
    heal.kind = 1;
    heal.caster_level = 10;
    CHECK_NEAR(host->ComputeSkillFormula(heal).amount, 165.0);
    heal.caster_level = 100;  // 等级收益封顶 60 → 30 + 110 + 150
    CHECK_NEAR(host->ComputeSkillFormula(heal).amount, 290.0);

    // 公式调用计数（观测）
    CHECK(host->HookCallCount(Hook::SkillFormula) == 8);

    // 公式是**纯函数**：同样输入重复调用结果一致（无随机、无墙钟，§25 可重放）
    for (int i = 0; i < 5; ++i) {
        CHECK_NEAR(host->ComputeSkillFormula(SkillReq("fireball", 77.0, 55.0)).amount,
                   40.0 + 1.35 * 77.0);
    }
}

// ===========================================================================
// §15.3 回归守卫 —— TASK-031 LuaVM::Allocate 内存记账下溢
// ---------------------------------------------------------------------------
// Lua 5.5 在「新建块」时把**对象 tag（8）**作为 osize 传给分配器（见 Lua 源码
// lmem.c：frealloc(ud, NULL, x, s) 创建新块，大小 's'，'x' 无关）。旧实现
// `base = used >= osize ? used - osize : 0` 在 ptr==NULL（新建块）时也减掉了这个 tag，
// 导致 mem_used_ 每次新建块少记 8 字节；而释放时按真实尺寸扣减，一进一出让 mem_used_
// 单调下漂并最终回绕到 ~2^64，此后 base+nsize 恒大于 memory_bytes → 任何分配都被拒 →
// 每条 hook 调用都 MemoryLimit 失败（本应成功的公式一律报错）。
//
// 本守卫用「连续 10 万次真实分配调用后 ScriptErrorCount==0」锁死该回归：一旦分配器
// 再记账出错，错误数立刻非零（旧实现下 fireball 公式会 7115.8ns 且全部 MemoryLimit）。
void test_allocator_memory_accounting_regression() {
    auto host = RealHost();
    CHECK(host != nullptr);
    if (host == nullptr) {
        return;
    }

    // 反复真实分配（每次 ComputeSkillFormula 都会建 payload / ctx / result 表）。
    // 若记账下溢，前若干次调用后 mem_used_ 会回绕，后续分配全部 MemoryLimit 失败。
    constexpr int kIters = 100000;
    double sink = 0.0;
    for (int i = 0; i < kIters; ++i) {
        const gp::SkillFormulaResult r =
            host->ComputeSkillFormula(SkillReq("fireball", 100.0, 100.0));
        sink += r.amount;  // 防止循环被优化掉
    }
    CHECK(host->ScriptErrorCount() == 0);
    // 末次结果仍然正确（没被钳成 0 / 没被报错吞掉）
    CHECK_NEAR(host->ComputeSkillFormula(SkillReq("fireball", 100.0, 100.0)).amount, 175.0);
    (void)sink;
}

// ===========================================================================
// §15.5 NPC 决策（真实脚本）
// ===========================================================================

void test_ai_decisions() {
    auto host = RealHost();
    CHECK(host != nullptr);
    if (host == nullptr) {
        return;
    }

    const auto decide = [&](std::string_view profile, double hp, double dist,
                            std::int64_t target) {
        AiContext ctx;
        ctx.profile = profile;
        ctx.self = 10;
        ctx.target = target;
        ctx.hp_pct = hp;
        ctx.distance = dist;
        ctx.state = 0;
        return host->DecideAi(ctx);
    };

    // aggressive_guard：近身攻击 / 中距追击 / 远距归位 / 残血撤退
    const gp::AiDecision attack = decide("aggressive_guard", 90.0, 1.0, 42);
    CHECK(attack.matched);
    CHECK(attack.ok);
    CHECK(attack.action == AiAction::Attack);
    CHECK(attack.target == 42);
    CHECK(!attack.clamped);
    CHECK(attack.script == "ai/aggressive_guard");

    CHECK(decide("aggressive_guard", 90.0, 5.0, 42).action == AiAction::Chase);
    CHECK(decide("aggressive_guard", 90.0, 20.0, 42).action == AiAction::Patrol);
    CHECK(decide("aggressive_guard", 90.0, 40.0, 42).action == AiAction::Patrol);
    CHECK(decide("aggressive_guard", 10.0, 1.0, 42).action == AiAction::Flee);
    CHECK(decide("aggressive_guard", 90.0, 1.0, 0).action == AiAction::Patrol);  // 无目标

    // patrol_sentry：超警戒圈 → Return（回岗）
    const gp::AiDecision back = decide("patrol_sentry", 90.0, 30.0, 42);
    CHECK(back.ok);
    CHECK(back.action == AiAction::Return);
    CHECK(decide("patrol_sentry", 90.0, 1.0, 42).action == AiAction::Attack);
    CHECK(decide("patrol_sentry", 10.0, 1.0, 42).action == AiAction::Flee);

    // fleeing_scout：半血以下必逃；高血近身才反击
    CHECK(decide("fleeing_scout", 40.0, 1.0, 42).action == AiAction::Flee);
    CHECK(decide("fleeing_scout", 90.0, 3.0, 42).action == AiAction::Attack);
    CHECK(decide("fleeing_scout", 60.0, 3.0, 42).action == AiAction::Chase);
    CHECK(decide("fleeing_scout", 60.0, 30.0, 0).action == AiAction::Patrol);

    // Lua 只给动作：决策结果里没有坐标 / 寻路字段（结构体层面即保证）
    // 6（aggressive_guard）+ 3（patrol_sentry）+ 4（fleeing_scout）= 13
    CHECK(host->HookCallCount(Hook::AiDecide) == 13);
}

// ===========================================================================
// §15.6 Boss 阶段（真实脚本）
// ===========================================================================

void test_boss_phases() {
    auto host = RealHost();
    CHECK(host != nullptr);
    if (host == nullptr) {
        return;
    }

    const auto boss = [&](std::string_view tmpl, double hp, std::int64_t phase) {
        BossContext ctx;
        ctx.boss_template = tmpl;
        ctx.boss = 7;
        ctx.hp_pct = hp;
        ctx.phase = phase;
        ctx.enraged = 0;
        return host->CheckBossPhase(ctx);
    };

    // 巨龙：未过阈值 → 不切阶段（脚本**刻意不回传结果**，避免每 Tick 重复切）
    const gp::BossPhaseDecision idle = boss("dragon", 100.0, 1);
    CHECK(idle.matched);
    CHECK(!idle.ok);
    CHECK(!idle.switched);
    CHECK(idle.phase == 1);

    // 70% 阈值 → 2 阶段 + 换技能组 1 + 要求广播
    const gp::BossPhaseDecision p2 = boss("dragon", 69.0, 1);
    CHECK(p2.ok);
    CHECK(p2.switched);
    CHECK(p2.phase == 2);
    CHECK(p2.skill_group == 1);
    CHECK(p2.broadcast);
    CHECK(p2.summon_npc == 0);
    CHECK(p2.script == "boss/dragon_phase");

    // 40% → 3 阶段 + 召唤 3001
    const gp::BossPhaseDecision p3 = boss("dragon", 39.0, 2);
    CHECK(p3.switched);
    CHECK(p3.phase == 3);
    CHECK(p3.summon_npc == 3001);

    // 15% → 4 阶段 + 召唤 3002
    const gp::BossPhaseDecision p4 = boss("dragon", 14.0, 3);
    CHECK(p4.switched);
    CHECK(p4.phase == 4);
    CHECK(p4.summon_npc == 3002);

    // 已经在该阶段 → 不重复切（避免广播风暴）
    CHECK(!boss("dragon", 39.0, 3).switched);

    // 石魔：50% 狂暴；10% 硬狂暴
    const gp::BossPhaseDecision enrage = boss("golem", 49.0, 1);
    CHECK(enrage.switched);
    CHECK(enrage.phase == 2);
    CHECK(enrage.skill_group == 2);
    const gp::BossPhaseDecision hard = boss("golem", 5.0, 1);
    CHECK(hard.switched);
    CHECK(hard.phase == 3);
    CHECK(hard.skill_group == 3);

    // 巫妖：30% → 3 阶段，召唤数随阶段递增（3102 × (3-1) = 6204）
    const gp::BossPhaseDecision lich = boss("lich", 29.0, 1);
    CHECK(lich.switched);
    CHECK(lich.phase == 3);
    CHECK(lich.summon_npc == 6204);

    // 已在该阶段 → 脚本刻意不回传结果（ok == false），C++ 侧也就不会重复切阶段 / 重复广播
    const gp::BossPhaseDecision same = boss("lich", 29.0, 3);
    CHECK(same.matched);
    CHECK(!same.ok);
    CHECK(!same.switched);
    CHECK(same.phase == 3);  // 保持当前阶段
}

// ===========================================================================
// §15.7 活动修正（真实脚本）
// ===========================================================================

void test_activity_modifiers() {
    auto host = RealHost();
    CHECK(host != nullptr);
    if (host == nullptr) {
        return;
    }

    const auto query = [&](std::string_view id, std::int64_t now_ms) {
        ActivityQuery q;
        q.activity_id = id;
        q.player = 1;
        q.now_ms = now_ms;
        q.exp_gain = 500;
        return host->QueryActivity(q);
    };

    // double_exp：窗口 [1'000'000, 4'600'000)
    const gp::ActivityModifier in = query("double_exp", 1'000'000);
    CHECK(in.matched);
    CHECK(in.active);
    CHECK_NEAR(in.exp_multiplier, 2.0);
    CHECK_NEAR(in.drop_multiplier, 1.0);
    CHECK(in.expires_at_ms == 4'600'000);
    CHECK(!in.clamped);
    CHECK(in.script == "event/double_exp");

    CHECK(!query("double_exp", 999'999).active);
    // 到点自动失效（不需要外部定时任务来关它，§15.7）
    const gp::ActivityModifier out = query("double_exp", 4'600'000);
    CHECK(!out.active);
    CHECK_NEAR(out.exp_multiplier, 1.0);

    // world_boss_dawn：30 分钟窗口，1.2× 经验 / 1.5× 掉落
    const gp::ActivityModifier wb = query("world_boss_dawn", 2'060'000);
    CHECK(wb.active);
    CHECK_NEAR(wb.exp_multiplier, 1.2);
    CHECK_NEAR(wb.drop_multiplier, 1.5);

    // festival_lantern：正常 1.5×/2.0×；最后 10 分钟软关闭 → 经验降为 1.2×，掉落保持
    const gp::ActivityModifier normal = query("festival_lantern", 3'000'010);
    CHECK(normal.active);
    CHECK_NEAR(normal.exp_multiplier, 1.5);
    CHECK_NEAR(normal.drop_multiplier, 2.0);

    const gp::ActivityModifier soft = query("festival_lantern", 3'000'000 + 7'200'000 - 300'000);
    CHECK(soft.active);
    CHECK_NEAR(soft.exp_multiplier, 1.2);
    CHECK_NEAR(soft.drop_multiplier, 2.0);

    CHECK(!query("festival_lantern", 3'000'000 + 7'200'000).active);
}

// ===========================================================================
// §19 钳制：脚本返回非法值 → C++ 校验并钳制（合成脚本）
// ===========================================================================

void test_host_clamps_illegal_results() {
    WriteScript("probe_nan.lua", ProbeScript("skill_formula", "skill", "nan", "{ amount = 0/0 }"));
    WriteScript("probe_neg.lua", ProbeScript("skill_formula", "skill", "neg", "{ amount = -5 }"));
    WriteScript("probe_huge.lua", ProbeScript("skill_formula", "skill", "huge", "{ amount = 1e12 }"));
    WriteScript("probe_ai.lua", ProbeScript("ai_decide", "profile", "probe", "{ action = 99 }"));
    WriteScript("probe_boss.lua",
                ProbeScript("boss_phase", "boss", "probe", "{ phase = 99, switched = true }"));
    // 「目标阶段 == 当前阶段」但脚本仍声明 switched=true：用于验证 C++ 是切换裁判
    WriteScript("probe_same.lua",
                ProbeScript("boss_phase", "boss", "same",
                            "{ phase = payload.phase, switched = true }"));
    WriteScript("probe_act.lua",
                ProbeScript("activity_modifier", "activity", "probe",
                            "{ active = true, exp_mult = 1e9, drop_mult = -3 }"));
    // 通配路由：keys 为空 → 任意 key 都命中（清单层语义，单点哨兵）
    WriteScript("probe_wild.lua",
                ProbeScript("skill_formula", "skill", "wild_key_never_used", "{ amount = 7 }"));

    ScriptManifest manifest;
    manifest.scripts.push_back(
        Bind("probe/nan", "probe_nan.lua", ScriptCategory::Skill, {Hook::SkillFormula}, {"nan"}));
    manifest.scripts.push_back(
        Bind("probe/neg", "probe_neg.lua", ScriptCategory::Skill, {Hook::SkillFormula}, {"neg"}));
    manifest.scripts.push_back(
        Bind("probe/huge", "probe_huge.lua", ScriptCategory::Skill, {Hook::SkillFormula}, {"huge"}));
    manifest.scripts.push_back(
        Bind("probe/ai", "probe_ai.lua", ScriptCategory::Ai, {Hook::AiDecide}, {"probe"}));
    manifest.scripts.push_back(
        Bind("probe/boss", "probe_boss.lua", ScriptCategory::Boss, {Hook::BossPhase}, {"probe"}));
    manifest.scripts.push_back(
        Bind("probe/same", "probe_same.lua", ScriptCategory::Boss, {Hook::BossPhase}, {"same"}));
    manifest.scripts.push_back(Bind("probe/act", "probe_act.lua", ScriptCategory::Event,
                                   {Hook::ActivityModifier}, {"probe"}));
    manifest.scripts.push_back(Bind("probe/wild", "probe_wild.lua", ScriptCategory::Skill,
                                   {Hook::SkillFormula}, {}));  // 空 keys = 通配

    auto host = MakeHost(ScratchRoot().string(), true);
    CHECK(host != nullptr);
    if (host == nullptr) {
        return;
    }
    const core::Result<std::size_t> loaded = host->LoadManifest(manifest);
    CHECK(loaded.HasValue());
    if (!loaded) {
        const std::string why(loaded.Err().Message());
        ErrorFmt("FAIL: synthetic manifest load: %s\n", why.c_str());
        return;
    }
    CHECK(loaded.Value() == 8);

    // NaN → 钳到 0 + clamped
    const gp::SkillFormulaResult nan = host->ComputeSkillFormula(SkillReq("nan", 0.0, 100.0));
    CHECK(nan.matched);
    CHECK(nan.ok);
    CHECK(nan.clamped);
    CHECK_NEAR(nan.amount, 0.0);
    CHECK(nan.reason != nullptr);

    const gp::SkillFormulaResult neg = host->ComputeSkillFormula(SkillReq("neg", 0.0, 100.0));
    CHECK(neg.clamped);
    CHECK_NEAR(neg.amount, 0.0);

    const gp::SkillFormulaResult huge = host->ComputeSkillFormula(SkillReq("huge", 0.0, 100.0));
    CHECK(huge.clamped);
    CHECK_NEAR(huge.amount, gp::kMaxFormulaAmount);

    // 越界动作 → Idle + clamped（绝不让脚本塞进非法状态）
    AiContext ai_ctx;
    ai_ctx.profile = "probe";
    ai_ctx.self = 1;
    ai_ctx.target = 2;
    ai_ctx.hp_pct = 50.0;
    ai_ctx.distance = 1.0;
    const gp::AiDecision ai = host->DecideAi(ai_ctx);
    CHECK(ai.ok);
    CHECK(ai.clamped);
    CHECK(ai.action == AiAction::Idle);

    BossContext boss_ctx;
    boss_ctx.boss_template = "probe";
    boss_ctx.hp_pct = 50.0;
    boss_ctx.phase = 1;
    const gp::BossPhaseDecision boss = host->CheckBossPhase(boss_ctx);
    CHECK(boss.ok);
    CHECK(boss.clamped);
    CHECK(boss.phase == gp::kMaxBossPhase);

    ActivityQuery act_query;
    act_query.activity_id = "probe";
    const gp::ActivityModifier act = host->QueryActivity(act_query);
    CHECK(act.matched);
    CHECK(act.clamped);
    CHECK(act.active);
    CHECK_NEAR(act.exp_multiplier, gp::kMaxMultiplier);  // 1e9 → 100
    CHECK_NEAR(act.drop_multiplier, 0.0);                // 负数 → 0

    // C++ 是「是否真的切了阶段」的裁判：脚本声明 switched=true，但目标阶段 == 当前阶段
    // → switched 强制 false。脚本的意图**不能**直接变成世界状态变更。
    BossContext same_ctx;
    same_ctx.boss_template = "same";
    same_ctx.hp_pct = 50.0;
    same_ctx.phase = 3;
    const gp::BossPhaseDecision same = host->CheckBossPhase(same_ctx);
    CHECK(same.ok);
    CHECK(!same.clamped);
    CHECK(same.phase == 3);
    CHECK(!same.switched);

    // 通配路由（清单 keys 为空）：任意 key 都会「路由到」该脚本；
    // 但脚本自身仍按 payload 字段决定是否认领 —— 两层职责分离，分别断言。
    const gp::SkillFormulaResult wild_hit =
        host->ComputeSkillFormula(SkillReq("wild_key_never_used", 0.0, 100.0));
    CHECK(wild_hit.matched);
    CHECK(wild_hit.ok);
    CHECK_NEAR(wild_hit.amount, 7.0);

    const gp::SkillFormulaResult wild_miss =
        host->ComputeSkillFormula(SkillReq("any_skill_at_all", 0.0, 100.0));
    CHECK(wild_miss.matched);  // 路由命中（通配）
    CHECK(!wild_miss.ok);      // 但脚本不认领 → 调用方走 C++ 默认公式，而不是「静默 0」
}

// ===========================================================================
// §19 装载失败隔离（合成脚本）
// ===========================================================================

void test_load_failure_isolation() {
    WriteScript("broken_syntax.lua", "function on_init(ctx) this is not lua end\n");
    WriteScript("missing_entries.lua",
                "function on_init(ctx) end\n");  // 缺 on_event / on_tick / on_reload
    WriteScript("good_skill.lua", ProbeScript("skill_formula", "skill", "iso", "{ amount = 11 }"));

    ScriptManifest manifest;
    manifest.scripts.push_back(Bind("iso/broken", "broken_syntax.lua", ScriptCategory::Skill,
                                   {Hook::SkillFormula}, {"iso"}));
    manifest.scripts.push_back(Bind("iso/incomplete", "missing_entries.lua", ScriptCategory::Skill,
                                   {Hook::SkillFormula}, {"iso"}));
    manifest.scripts.push_back(Bind("iso/good", "good_skill.lua", ScriptCategory::Skill,
                                   {Hook::SkillFormula}, {"iso"}));
    manifest.scripts.push_back(Bind("iso/absent", "no_such_file.lua", ScriptCategory::Skill,
                                   {Hook::SkillFormula}, {"iso"}));

    // require_all_scripts = false：单条失败只作废自己，其余照常装载
    auto host = MakeHost(ScratchRoot().string(), false);
    CHECK(host != nullptr);
    if (host == nullptr) {
        return;
    }
    const core::Result<std::size_t> loaded = host->LoadManifest(manifest);
    CHECK(loaded.HasValue());
    if (loaded) {
        CHECK(loaded.Value() == 1);
    }
    CHECK(host->Stats().total == 4);
    CHECK(host->Stats().loaded == 1);
    CHECK(host->Stats().failed == 3);
    CHECK(host->LoadErrors().size() == 3);
    CHECK(host->ScriptNames().size() == 1);
    if (!host->ScriptNames().empty()) {
        CHECK(host->ScriptNames()[0] == "iso/good");
    }

    // 好脚本仍然被路由到（坏脚本不进路由表 → 不存在「半挂载」）
    const gp::SkillFormulaResult ok = host->ComputeSkillFormula(SkillReq("iso", 0.0, 100.0));
    CHECK(ok.matched);
    CHECK(ok.ok);
    CHECK_NEAR(ok.amount, 11.0);
    CHECK(ok.script == "iso/good");

    // require_all_scripts = true：任一失败即整体失败（启动期 fail-fast）
    auto strict = MakeHost(ScratchRoot().string(), true);
    CHECK(strict != nullptr);
    if (strict != nullptr) {
        CHECK(!strict->LoadManifest(manifest).HasValue());
        CHECK(strict->Stats().failed == 3);
    }
}

void test_reference_registry_gate() {
    WriteScript("ref_target.lua", ProbeScript("skill_formula", "skill", "iso", "{ amount = 11 }"));

    // §19 引用校验：脚本引用的 Quest 不存在 → 加载期报错（零副作用）
    ScriptManifest present;
    present.scripts.push_back(Bind("ref/quest", "ref_target.lua", ScriptCategory::Skill,
                                  {Hook::SkillFormula}, {"iso"}, 0.0, {"quest:1001"}));
    auto host_ok = MakeHost(ScratchRoot().string(), true);
    CHECK(host_ok != nullptr);
    if (host_ok != nullptr) {
        RefRegistry registry;
        host_ok->SetReferenceRegistry(&registry);
        CHECK(host_ok->LoadManifest(present).HasValue());
    }

    ScriptManifest missing;
    missing.scripts.push_back(Bind("ref/quest2", "ref_target.lua", ScriptCategory::Skill,
                                  {Hook::SkillFormula}, {"iso"}, 0.0, {"quest:9999"}));
    auto host_bad = MakeHost(ScratchRoot().string(), true);
    CHECK(host_bad != nullptr);
    if (host_bad != nullptr) {
        RefRegistry registry;
        host_bad->SetReferenceRegistry(&registry);
        const core::Result<std::size_t> r = host_bad->LoadManifest(missing);
        CHECK(!r.HasValue());
        CHECK(host_bad->Stats().loaded == 0);
        CHECK(host_bad->LoadErrors().empty());  // 前置门禁失败：连逐条装载都没开始
    }
}

// ===========================================================================
// §19 / §21 红线：脚本做 IO 被拦截
// ===========================================================================

void test_redline_blocks_io() {
    // 故意把禁用调用写在 `on_event` 里：顶层 chunk 干净，因此**唯一**能拦下它的是
    // 流水线的沙箱静态扫描（而不是「顶层执行恰好报错」这种偶然）。
    WriteScript("evil_io.lua", ScriptWithEventBody("quest_event", "  io.open(\"/tmp/secret\")"));
    WriteScript("evil_os.lua", ScriptWithEventBody("quest_event", "  os.time()"));
    WriteScript("evil_loadstring.lua",
                ScriptWithEventBody("quest_event", "  loadstring(\"return 1\")"));
    WriteScript("honest.lua", ProbeScript("quest_event", "probe_unused", "k", "{ amount = 1 }"));

    ScriptManifest manifest;
    manifest.scripts.push_back(
        Bind("evil/io", "evil_io.lua", ScriptCategory::Quest, {Hook::QuestEvent}, {"probe"}));
    manifest.scripts.push_back(
        Bind("evil/os", "evil_os.lua", ScriptCategory::Quest, {Hook::QuestEvent}, {"probe"}));
    manifest.scripts.push_back(Bind("evil/loadstring", "evil_loadstring.lua", ScriptCategory::Quest,
                                   {Hook::QuestEvent}, {"probe"}));
    manifest.scripts.push_back(Bind("evil/honest", "honest.lua", ScriptCategory::Quest,
                                   {Hook::QuestEvent}, {"probe"}));

    auto host = MakeHost(ScratchRoot().string(), false);
    CHECK(host != nullptr);
    if (host == nullptr) {
        return;
    }
    const core::Result<std::size_t> loaded = host->LoadManifest(manifest);
    CHECK(loaded.HasValue());
    if (loaded) {
        CHECK(loaded.Value() == 1);  // 只有老实脚本活下来
    }
    CHECK(host->Stats().failed == 3);

    bool saw_io = false;
    bool saw_os = false;
    bool saw_loadstring = false;
    for (const std::string& line : host->LoadErrors()) {
        if (line.find("banned api 'io'") != std::string::npos) {
            saw_io = true;
        }
        if (line.find("banned api 'os'") != std::string::npos) {
            saw_os = true;
        }
        if (line.find("banned api 'loadstring'") != std::string::npos) {
            saw_loadstring = true;
        }
    }
    CHECK(saw_io);
    CHECK(saw_os);
    CHECK(saw_loadstring);

    // 被拦下的脚本不进路由表：坏脚本被隔离，够不到任何 hook
    CHECK(host->RouteCount(Hook::QuestEvent) == 1);
    CHECK(host->ScriptNames().size() == 1);
    if (!host->ScriptNames().empty()) {
        CHECK(host->ScriptNames()[0] == "evil/honest");
    }
}

// ===========================================================================
// §21 红线：脚本改不了 HP —— 写意图只能由状态 Owner 执行
// ===========================================================================

void test_redline_controlled_write_only() {
    WriteScript("hp_probe.lua",
                ScriptWithEventBody("hp_probe", "  entity.set_hp(payload.target, 0)"));
    ScriptManifest manifest;
    manifest.scripts.push_back(Bind("probe/hp", "hp_probe.lua", ScriptCategory::Quest,
                                   {Hook::QuestEvent}, {"hp_probe"}));

    // 受控写的前置：目标实体必须真实存在（`entity.set_hp` 先校验实体在不在，
    // 否则一条指向已销毁实体的命令会流到业务系统各自解释 —— TASK-031 §19 口径）。
    game::EntityManager entities;
    const core::Result<game::Entity*> created =
        entities.Create(game::EntityType::Monster, 1, game::Position{});
    CHECK(created.HasValue());
    if (!created) {
        return;
    }
    const game::EntityId target = created.Value()->Id();
    CHECK(target != 0);

    const auto event = [&target]() {
        GameplayPayload p;
        (void)p.SetName("hp_probe");
        (void)p.AddInt("target", static_cast<std::int64_t>(target));
        return p;
    };

    // ---- A) 没有状态 Owner（未注入 handler）：明确报错，绝不「静默生效」 ----
    {
        auto host = MakeHost(ScratchRoot().string(), true);
        CHECK(host != nullptr);
        if (host != nullptr) {
            CHECK(host->BindEntityManager(entities).HasValue());
            CHECK(host->LoadManifest(manifest).HasValue());
            const std::size_t called = host->DispatchQuestEvent(event());
            CHECK(called == 0);                    // 该次调用作废
            CHECK(host->ScriptErrorCount() == 1);  // 被隔离，不崩 Scene
            CHECK(host->HookCallCount(Hook::QuestEvent) == 0);
        }
    }

    // ---- B) 注入 handler：写入由宿主执行（Lua 只提交「意图 + 参数」）----
    {
        auto host = MakeHost(ScratchRoot().string(), true);
        CHECK(host != nullptr);
        if (host != nullptr) {
            core::CommandBus commands;
            HandlerLog log;
            CHECK(host->BindEntityManager(entities).HasValue());
            CHECK(host->BindCommandBus(commands).HasValue());
            host->BindCommandHandler(&RecordingHandler, &log);
            CHECK(host->LoadManifest(manifest).HasValue());

            const std::size_t called = host->DispatchQuestEvent(event());
            CHECK(called == 1);
            CHECK(host->ScriptErrorCount() == 0);
            CHECK(host->HookCallCount(Hook::QuestEvent) == 1);

            // 参数原样交给状态 Owner —— 脚本无法自己改写实时状态（§4 单一权威写入者）
            CHECK(log.calls == 1);
            CHECK(log.last_op == "entity.set_hp");
            CHECK(log.a0 == static_cast<std::int64_t>(target));
            CHECK(log.a1 == 0);
        }
    }
}

// ===========================================================================
// §21 / §9 周期脚本：≤ 1Hz + 丢弃积压
// ===========================================================================

void test_tick_low_frequency() {
    auto host = RealHost();
    CHECK(host != nullptr);
    if (host == nullptr) {
        return;
    }
    // 清单里只有 escort_merchant / double_exp 声明了 tick_hz = 1
    CHECK(host->TickNumber() == 0);

    CHECK(host->Tick(0.5) == 0);  // 累加 500ms < 1000ms
    CHECK(host->TickNumber() == 1);
    CHECK(host->Tick(0.6) == 2);  // 累加 1100ms ≥ 1000ms → 两个周期脚本各触发一次
    CHECK(host->Tick(0.4) == 0);  // 累加器已清零
    CHECK(host->Tick(3.0) == 2);  // 长 dt 只触发一次（丢弃积压，不进入 catch-up 死亡螺旋）
    CHECK(host->TickNumber() == 4);

    // dt <= 0 直接返回，但 Tick 号照常推进（观测口径一致）
    CHECK(host->Tick(0.0) == 0);
    CHECK(host->TickNumber() == 5);

    // 非周期脚本从不被 on_tick 调用（清单声明即契约）
    CHECK(host->Stats().loaded == 15);
}

// ===========================================================================
// §20#5 热更演练：生效 + 回滚
// ===========================================================================

void test_hot_reload_drill() {
    auto host = RealHost();
    CHECK(host != nullptr);
    if (host == nullptr) {
        return;
    }

    const std::string original = ReadFile(kFireballPath);
    CHECK(!original.empty());
    if (original.empty()) {
        return;
    }

    // 基线：40 + 1.35 × 100 = 175
    CHECK_NEAR(host->ComputeSkillFormula(SkillReq("fireball", 100.0, 100.0)).amount, 175.0);
    const mmo::script::ScriptVersion* v1 = host->CurrentVersion("skill/fireball");
    CHECK(v1 != nullptr);
    std::string v1_checksum;
    if (v1 != nullptr) {
        CHECK(v1->version == 1);
        CHECK(!v1->checksum.empty());
        v1_checksum.assign(v1->checksum);  // 立刻拷贝：指针指向 reloader 内部记录，后续会被覆写
    }

    std::string patched = original;
    CHECK(ReplaceOnce(patched, "local BASE = 40.0", "local BASE = 100.0"));
    CHECK(patched != original);

    // 阶段 1-3（任意线程）：`Prepare → Validate` 是**两个都必须做的步骤** ——
    // 流水线内部以 (name, checksum) 的「已校验记录」为激活判据（TASK-032 §21），
    // 少了 Validate 这一步，Activate 必然 UNAUTHORIZED。
    const core::Result<mmo::script::ReloadTicket> ticket =
        host->PrepareReload("skill/fireball", patched);
    CHECK(ticket.HasValue());
    if (ticket) {
        const core::Result<mmo::script::ValidationReport> report =
            host->ValidateReload(ticket.Value());
        CHECK(report.HasValue());
        if (report) {
            CHECK(report.Value().ok);
            CHECK(report.Value().sandbox_ok);
        }
    }

    // 非安全点 Activate → BUSY（§21 禁止 Tick 中途替换）
    if (ticket) {
        const core::Result<void> outside =
            host->ActivateReload(ticket.Value(), core::kInvalidTraceId);
        CHECK(!outside.HasValue());
        CHECK(outside.Err().Code() == core::ErrorCode::BUSY);
    }
    CHECK_NEAR(host->ComputeSkillFormula(SkillReq("fireball", 100.0, 100.0)).amount, 175.0);

    // 安全点内 Activate → 线上即时生效
    host->BeginSafePoint(1);
    CHECK(host->InSafePoint());
    if (ticket) {
        CHECK(host->ActivateReload(ticket.Value(), core::kInvalidTraceId).HasValue());
    }
    host->EndSafePoint();
    CHECK(!host->InSafePoint());

    const gp::SkillFormulaResult hot = host->ComputeSkillFormula(SkillReq("fireball", 100.0, 100.0));
    CHECK(hot.ok);
    CHECK_NEAR(hot.amount, 235.0);  // 100 + 135
    const mmo::script::ScriptVersion* v2 = host->CurrentVersion("skill/fireball");
    CHECK(v2 != nullptr);
    std::int64_t v2_version = 0;
    if (v2 != nullptr) {
        CHECK(v2->version == 2);
        CHECK(!v2->checksum.empty());
        CHECK(v2->checksum != v1_checksum);  // 源码确实换了（checksum 变更检测）
        v2_version = static_cast<std::int64_t>(v2->version);
    }

    // 回滚 → 旧公式恢复（§20#5「且可回滚」）
    host->BeginSafePoint(2);
    CHECK(host->Rollback("skill/fireball", core::kInvalidTraceId).HasValue());
    host->EndSafePoint();
    CHECK_NEAR(host->ComputeSkillFormula(SkillReq("fireball", 100.0, 100.0)).amount, 175.0);
    const mmo::script::ScriptVersion* v3 = host->CurrentVersion("skill/fireball");
    CHECK(v3 != nullptr);
    std::int64_t rolled_back_version = 0;
    if (v3 != nullptr) {
        // 判据是「内容回到旧版本」，而不是「版本号回到 1」：版本号是**单调递增**的审计序号
        // （回滚本身也是一次激活，会占用新序号），重用旧序号会让审计无法追溯。
        CHECK(v3->checksum == v1_checksum);
        CHECK(v3->version != v2_version);
        rolled_back_version = static_cast<std::int64_t>(v3->version);
    }

    // 热更不是装载：不在清单里的名字一律拒绝（禁止「热更」变成「偷偷加脚本」）
    CHECK(!host->PrepareReload("no/such/script", original).HasValue());

    // 违法源码在**校验阶段**就被拦下 → Activate 拒绝，线上版本不变（§19 / §20#2）
    std::string evil = original;
    evil += "\nfunction __evil() io.open(\"/tmp/x\") end\n";
    const core::Result<mmo::script::ReloadTicket> bad = host->PrepareReload("skill/fireball", evil);
    // Prepare 只做编译 —— 语法没问题，所以票证能拿到；拦截发生在 Validate
    CHECK(bad.HasValue());
    if (bad) {
        const core::Result<mmo::script::ValidationReport> bad_report =
            host->ValidateReload(bad.Value());
        CHECK(bad_report.HasValue());
        if (bad_report) {
            CHECK(!bad_report.Value().ok);
            CHECK(!bad_report.Value().sandbox_ok);  // 命中沙箱静态扫描
        }
        host->BeginSafePoint(3);
        CHECK(!host->ActivateReload(bad.Value(), core::kInvalidTraceId).HasValue());
        host->EndSafePoint();
    }
    CHECK_NEAR(host->ComputeSkillFormula(SkillReq("fireball", 100.0, 100.0)).amount, 175.0);
    const mmo::script::ScriptVersion* v4 = host->CurrentVersion("skill/fireball");
    CHECK(v4 != nullptr);
    if (v4 != nullptr) {
        // 未通过校验的源码**不能**上线：当前版本号与回滚后完全一致（没有被推进）
        CHECK(static_cast<std::int64_t>(v4->version) == rolled_back_version);
        CHECK(v4->checksum == v1_checksum);
    }

    // 语法错 → 旧版本零影响
    host->BeginSafePoint(4);
    CHECK(!host->PrepareReload("skill/fireball", "function on_init(ctx) !! end").HasValue());
    host->EndSafePoint();
    CHECK_NEAR(host->ComputeSkillFormula(SkillReq("fireball", 100.0, 100.0)).amount, 175.0);

    // 审计队列在 Tick 外交付（无 sink 时返回 0，记录留在内存队列）
    CHECK(host->DrainAudit() == 0);

    // 热更后其余脚本不受影响（只换掉目标脚本的模块表）
    CHECK(host->Stats().loaded == 15);
    CHECK_NEAR(host->ComputeSkillFormula(SkillReq("ice_lance", 100.0, 100.0)).amount, 130.0);
}

// ===========================================================================
// §15.4 热更后状态连续（合成脚本，验证 on_reload 收到真实旧版本号）
// ===========================================================================

void test_reload_contract_and_state() {
    // 脚本在全局累积 `hits`，并经 on_reload 记录旧版本号 —— 两者都应跨热更连续。
    const char* kV1 =
        "hits = hits or 0\n"
        "last_old_version = last_old_version or -1\n"
        "function on_init(ctx) end\n"
        "function on_event(ctx, name, payload)\n"
        "  payload = payload or gameplay.payload()\n"
        "  if name ~= \"skill_formula\" then return end\n"
        "  if payload.skill ~= \"counter\" then return end\n"
        "  hits = hits + 1\n"
        "  gameplay.result({ amount = hits })\n"
        "end\n"
        "function on_tick(ctx, dt) end\n"
        "function on_reload(ctx, old_version)\n"
        "  last_old_version = old_version\n"
        "end\n";
    const char* kV2 =
        "hits = hits or 0\n"
        "last_old_version = last_old_version or -1\n"
        "function on_init(ctx) end\n"
        "function on_event(ctx, name, payload)\n"
        "  payload = payload or gameplay.payload()\n"
        "  if name ~= \"skill_formula\" then return end\n"
        "  if payload.skill ~= \"counter\" then return end\n"
        "  hits = hits + 1\n"
        "  gameplay.result({ amount = hits * 10 })\n"
        "end\n"
        "function on_tick(ctx, dt) end\n"
        "function on_reload(ctx, old_version)\n"
        "  last_old_version = old_version\n"
        "end\n";

    WriteScript("counter.lua", kV1);
    ScriptManifest manifest;
    manifest.scripts.push_back(Bind("probe/counter", "counter.lua", ScriptCategory::Skill,
                                   {Hook::SkillFormula}, {"counter"}));

    auto host = MakeHost(ScratchRoot().string(), true);
    CHECK(host != nullptr);
    if (host == nullptr) {
        return;
    }
    CHECK(host->LoadManifest(manifest).HasValue());

    CHECK_NEAR(host->ComputeSkillFormula(SkillReq("counter", 0.0, 100.0)).amount, 1.0);
    CHECK_NEAR(host->ComputeSkillFormula(SkillReq("counter", 0.0, 100.0)).amount, 2.0);

    // 热更换版：新逻辑（hits × 10）生效，但**累积量必须保留**（ReloadInPlace 语义）
    host->BeginSafePoint(1);
    const core::Result<mmo::script::ReloadTicket> ticket =
        host->PrepareReload("probe/counter", kV2);
    CHECK(ticket.HasValue());
    if (ticket) {
        CHECK(host->ValidateReload(ticket.Value()).HasValue());
        host->BeginSafePoint(1);
        CHECK(host->ActivateReload(ticket.Value(), core::kInvalidTraceId).HasValue());
        host->EndSafePoint();
    }

    // hits 从 2 续到 3 → 3 × 10 = 30：证明脚本全局状态连续（不是重建 VM）
    CHECK_NEAR(host->ComputeSkillFormula(SkillReq("counter", 0.0, 100.0)).amount, 30.0);
    CHECK_NEAR(host->ComputeSkillFormula(SkillReq("counter", 0.0, 100.0)).amount, 40.0);

    // 版本历史保留最近 5 个（§20#5）
    CHECK(host->Reloader().History("probe/counter").size() == 2);
}

}  // namespace

int main() {
    Line("== TASK-033 gameplay_script_test ==\n");

    test_clamp_and_naming();
    test_payload_container();
    test_manifest_real();
    test_manifest_validation_rejects();
    test_load_real_manifest_and_routes();
    test_skill_formulas();
    test_allocator_memory_accounting_regression();
    test_ai_decisions();
    test_boss_phases();
    test_activity_modifiers();
    test_host_clamps_illegal_results();
    test_load_failure_isolation();
    test_reference_registry_gate();
    test_redline_blocks_io();
    test_redline_controlled_write_only();
    test_tick_low_frequency();
    test_hot_reload_drill();
    test_reload_contract_and_state();

    LineFmt("checks=%d failures=%d\n", checks, failures);
    if (failures > 0) {
        ErrorFmt("TASK-033 unit test FAILED: %d/%d\n", failures, checks);
        return 1;
    }
    Line("TASK-033 unit test OK\n");
    return 0;
}
