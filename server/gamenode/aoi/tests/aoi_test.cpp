// server/gamenode/aoi/tests/aoi_test.cpp — TASK-014 §16 / §19 / §20 验收 #2
//
// 输出统一走 mmo::core::test（test_print.h），禁止裸 std::cout / printf。
// ctest 标签：Aoi（验收脚本 run_ctest 'Aoi'）。

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <random>
#include <vector>

#include "test_print.h"
#include "mmo/core/error/result.h"
#include "mmo/game/aoi/aoi.h"
#include "mmo/game/aoi/dynamic_grid_aoi.h"

namespace {

using namespace mmo::game::aoi;
using mmo::core::test::ErrorFmt;
using mmo::core::test::Line;
using mmo::core::ErrorCode;

constexpr float kViewR = 50.0f;
constexpr float kCell = 20.0f;
constexpr float kWorld = 1000.0f;  // 随机布局范围 ±1000m

int g_fails = 0;

#define CHECK(cond, msg)                                                      \
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
        } else if (r_.Err().Code() != (expected)) {                           \
            ErrorFmt("FAIL: %s (line %d): code mismatch\n", msg, __LINE__);    \
            ++g_fails;                                                         \
        }                                                                      \
    } while (0)

float Dist3(const Position& a, const Position& b) {
    const float dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

// 暴力 O(N²) 参考实现：返回 id 视野内（距离 ≤ r，不含自身）实体，排序去重。
void BruteVisible(const std::vector<std::pair<EntityId, Position>>& ents, EntityId id,
                  float r, std::vector<EntityId>& out) {
    out.clear();
    Position p{};
    for (const auto& e : ents)
        if (e.first == id) { p = e.second; break; }
    for (const auto& e : ents) {
        if (e.first == id) continue;
        if (Dist3(p, e.second) <= r) out.push_back(e.first);
    }
    std::sort(out.begin(), out.end());
}

bool SetsEqual(const std::vector<EntityId>& a, const std::vector<EntityId>& b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i)
        if (a[i] != b[i]) return false;
    return true;
}

AoiConfig MakeCfg() {
    AoiConfig c;
    c.cell_size = kCell;
    c.view_radius = kViewR;
    c.max_entities = 10000;
    return c;
}

std::vector<std::pair<EntityId, Position>> RandomLayout(std::mt19937& rng, std::size_t n) {
    std::uniform_real_distribution<float> d(-kWorld, kWorld);
    std::uniform_real_distribution<float> dz(-5.0f, 5.0f);
    std::vector<std::pair<EntityId, Position>> out;
    out.reserve(n);
    for (std::size_t i = 0; i < n; ++i) {
        Position p{d(rng), d(rng), dz(rng), 0.0f};
        out.emplace_back(static_cast<EntityId>(i + 1), p);
    }
    return out;
}

// ---------------------------------------------------------------------------
// §16 / §20 验收 #2：QueryVisible 与暴力参考实现 100% 一致（1000 × 100 布局）
// ---------------------------------------------------------------------------
void TestBruteForceConsistency() {
    std::mt19937 rng(0x1234ABCDu);
    constexpr std::size_t N = 1000;
    constexpr int LAYOUTS = 100;

    std::vector<EntityId> qvis;
    std::vector<EntityId> bvis;
    std::size_t mismatches = 0;

    for (int L = 0; L < LAYOUTS; ++L) {
        auto layout = RandomLayout(rng, N);
        auto aoi = CreateDynamicGridAoi(MakeCfg());
        for (const auto& e : layout) CHECK(aoi->Enter(e.first, e.second).HasValue(), "enter");
        for (const auto& e : layout) {
            BruteVisible(layout, e.first, kViewR, bvis);
            CHECK(aoi->QueryVisible(e.first, qvis).HasValue(), "query");
            std::sort(qvis.begin(), qvis.end());
            if (!SetsEqual(qvis, bvis)) ++mismatches;
        }
    }
    CHECK(mismatches == 0, "QueryVisible == brute force (1000x100 layouts)");
}

// ---------------------------------------------------------------------------
// §16 基础语义：Enter/Leave/Move + 跨格检测 + 去重 + 同格多实体
// ---------------------------------------------------------------------------
void TestBasicSemantics() {
    auto aoi = CreateDynamicGridAoi(MakeCfg());
    // 同格多实体：都在原点附近
    CHECK(aoi->Enter(1, Position{0, 0, 0, 0}).HasValue(), "enter 1");
    CHECK(aoi->Enter(2, Position{5, 0, 0, 0}).HasValue(), "enter 2 (same cell)");
    CHECK(aoi->Enter(3, Position{0, 5, 0, 0}).HasValue(), "enter 3 (same cell)");
    CHECK(aoi->Enter(4, Position{500, 500, 0, 0}).HasValue(), "enter 4 (far, other cell)");

    std::vector<EntityId> v;
    CHECK(aoi->QueryVisible(1, v).HasValue(), "query 1");
    std::sort(v.begin(), v.end());
    CHECK(SetsEqual(v, {2, 3}), "entity 1 sees only near entities (dedup, excludes self & far)");

    // 跨格检测：把 4 从远格移到 1 附近
    auto mv = aoi->Move(4, Position{3, 0, 0, 0});
    CHECK(mv.HasValue(), "move 4 into view");
    CHECK(mv.Value().entered.size() == 3, "move 4 entered 1/2/3");
    CHECK(mv.Value().left.empty(), "move 4 left none");

    // 再查 1 应看到 2/3/4
    CHECK(aoi->QueryVisible(1, v).HasValue(), "re-query 1");
    std::sort(v.begin(), v.end());
    CHECK(SetsEqual(v, {2, 3, 4}), "entity 1 now sees 4 after move");

    // Leave 4
    CHECK(aoi->Leave(4).HasValue(), "leave 4");
    CHECK(aoi->QueryVisible(1, v).HasValue(), "re-query 1 after leave");
    std::sort(v.begin(), v.end());
    CHECK(SetsEqual(v, {2, 3}), "entity 1 sees 2/3 after 4 left");

    // 重复 Enter → INVALID_ARGUMENT
    CHECK_CODE(aoi->Enter(2, Position{1, 1, 0, 0}), ErrorCode::INVALID_ARGUMENT, "dup enter");
    // 未知 id 操作 → NOT_FOUND
    CHECK_CODE(aoi->Leave(999), ErrorCode::NOT_FOUND, "leave unknown");
    CHECK_CODE(aoi->Move(999, Position{0, 0, 0, 0}), ErrorCode::NOT_FOUND, "move unknown");
    CHECK_CODE(aoi->QueryVisible(999, v), ErrorCode::NOT_FOUND, "query unknown");
    CHECK_CODE(aoi->Broadcast(999, std::span<const std::uint8_t>{}), ErrorCode::NOT_FOUND,
               "broadcast unknown");
}

// ---------------------------------------------------------------------------
// §16 Move 增量 diff 与暴力 diff 一致
// ---------------------------------------------------------------------------
void TestMoveDiff() {
    std::mt19937 rng(0x55AA55AAu);
    constexpr std::size_t N = 400;
    auto layout = RandomLayout(rng, N);
    auto aoi = CreateDynamicGridAoi(MakeCfg());
    for (const auto& e : layout) CHECK(aoi->Enter(e.first, e.second).HasValue(), "enter");

    std::uniform_real_distribution<float> d(-kWorld, kWorld);
    std::vector<EntityId> old_b, new_b, qv;
    for (int step = 0; step < 200; ++step) {
        const EntityId id = static_cast<EntityId>((rng() % N) + 1);
        Position to{d(rng), d(rng), 0.0f, 0.0f};
        // 暴力：移动前/后可见集 diff
        BruteVisible(layout, id, kViewR, old_b);
        for (auto& e : layout)
            if (e.first == id) e.second = to;  // 同步参考实现的位置
        BruteVisible(layout, id, kViewR, new_b);

        auto mv = aoi->Move(id, to);
        CHECK(mv.HasValue(), "move");
        MoveResult r = std::move(mv).Value();  // 取出可变副本用于排序
        std::sort(r.entered.begin(), r.entered.end());
        std::sort(r.left.begin(), r.left.end());
        std::vector<EntityId> bf_entered, bf_left;
        std::set_difference(new_b.begin(), new_b.end(), old_b.begin(), old_b.end(),
                            std::back_inserter(bf_entered));
        std::set_difference(old_b.begin(), old_b.end(), new_b.begin(), new_b.end(),
                            std::back_inserter(bf_left));
        CHECK(SetsEqual(r.entered, bf_entered), "move entered == brute diff");
        CHECK(SetsEqual(r.left, bf_left), "move left == brute diff");

        // 移动后 QueryVisible 必须仍 == 暴力
        CHECK(aoi->QueryVisible(id, qv).HasValue(), "query after move");
        std::sort(qv.begin(), qv.end());
        CHECK(SetsEqual(qv, new_b), "post-move QueryVisible == brute");
    }
}

// ---------------------------------------------------------------------------
// §17 简化集成：500 实体随机游走 2000 步，抽样与暴力比对（每 100 步）
// ---------------------------------------------------------------------------
void TestRandomWalk() {
    std::mt19937 rng(0x0BADF00Du);
    constexpr std::size_t N = 500;
    auto layout = RandomLayout(rng, N);
    auto aoi = CreateDynamicGridAoi(MakeCfg());
    for (const auto& e : layout) CHECK(aoi->Enter(e.first, e.second).HasValue(), "enter");

    std::uniform_real_distribution<float> step(-15.0f, 15.0f);
    std::vector<EntityId> qv, bv;
    std::size_t mismatches = 0;
    for (int t = 0; t < 2000; ++t) {
        const EntityId id = static_cast<EntityId>((rng() % N) + 1);
        Position to{layout[id - 1].second.x + step(rng), layout[id - 1].second.y + step(rng),
                    0.0f, 0.0f};
        // 钳制到世界内（模拟合法移动）
        to.x = std::clamp(to.x, -kWorld, kWorld);
        to.y = std::clamp(to.y, -kWorld, kWorld);
        layout[id - 1].second = to;
        auto mv = aoi->Move(id, to);
        CHECK(mv.HasValue(), "walk move");
        if (t % 100 == 0) {
            for (const auto& e : layout) {
                BruteVisible(layout, e.first, kViewR, bv);
                CHECK(aoi->QueryVisible(e.first, qv).HasValue(), "walk query");
                std::sort(qv.begin(), qv.end());
                if (!SetsEqual(qv, bv)) ++mismatches;
            }
        }
    }
    CHECK(mismatches == 0, "random walk visible sets consistent with brute force");
}

// ---------------------------------------------------------------------------
// §19 Failure：NaN / 越界被拒；超容量 BUSY；空格查询空集不崩
// ---------------------------------------------------------------------------
void TestFailures() {
    auto aoi = CreateDynamicGridAoi(MakeCfg());
    // NaN 坐标
    CHECK_CODE(aoi->Enter(1, Position{std::nanf(""), 0, 0, 0}), ErrorCode::INVALID_ARGUMENT,
               "enter NaN x");
    CHECK_CODE(aoi->Enter(1, Position{0, std::nanf(""), 0, 0}), ErrorCode::INVALID_ARGUMENT,
               "enter NaN y");
    // 越界坐标（> kWorldHalf=1e6）
    CHECK_CODE(aoi->Enter(1, Position{2.0e6f, 0, 0, 0}), ErrorCode::INVALID_ARGUMENT,
               "enter oob x");
    CHECK_CODE(aoi->Enter(1, Position{0, -2.0e6f, 0, 0}), ErrorCode::INVALID_ARGUMENT,
               "enter oob y");
    // 负坐标合法（应成功）
    CHECK(aoi->Enter(1, Position{-100, -100, 0, 0}).HasValue(), "enter negative coord ok");
    CHECK_CODE(aoi->Move(1, Position{std::nanf(""), 0, 0, 0}), ErrorCode::INVALID_ARGUMENT,
               "move NaN");
    CHECK_CODE(aoi->Move(1, Position{5.0e6f, 0, 0, 0}), ErrorCode::INVALID_ARGUMENT,
               "move oob");

    // 超容量 BUSY
    AoiConfig sc = MakeCfg();
    sc.max_entities = 2;
    auto capped = CreateDynamicGridAoi(sc);
    CHECK(capped->Enter(1, Position{0, 0, 0, 0}).HasValue(), "capped enter 1");
    CHECK(capped->Enter(2, Position{1, 0, 0, 0}).HasValue(), "capped enter 2");
    CHECK_CODE(capped->Enter(3, Position{2, 0, 0, 0}), ErrorCode::BUSY, "capped enter 3 BUSY");

    // 空格查询：空 AOI 返回空集不崩
    auto empty = CreateDynamicGridAoi(MakeCfg());
    std::vector<EntityId> v;
    CHECK_CODE(empty->QueryVisible(1, v), ErrorCode::NOT_FOUND, "empty query unknown");
    CHECK(empty->Stats().entity_count == 0, "empty stats entity_count 0");
    CHECK(empty->Stats().cell_count == 0, "empty stats cell_count 0");
}

// ---------------------------------------------------------------------------
// §17/§20 Broadcast：投递数 == 可见集大小，无重复无遗漏（经 sink 计数）
// ---------------------------------------------------------------------------
void TestBroadcast() {
    auto aoi = std::make_unique<DynamicGridAoi>(MakeCfg());
    // 3 个近邻 + 1 个远邻
    (void)aoi->Enter(1, Position{0, 0, 0, 0});
    (void)aoi->Enter(2, Position{10, 0, 0, 0});
    (void)aoi->Enter(3, Position{0, 10, 0, 0});
    (void)aoi->Enter(4, Position{10, 10, 0, 0});
    (void)aoi->Enter(5, Position{500, 500, 0, 0});  // 远，不在 1 视野

    std::vector<int> recv_count(6, 0);  // recv_count[target] += 1
    std::vector<std::size_t> payload_seen(6, 0);
    aoi->SetBroadcastSink([&](EntityId from, EntityId to, std::span<const std::uint8_t> pl) {
        if (from != 1) return;  // 只统计从 1 发出的
        ++recv_count[static_cast<int>(to)];
        payload_seen[static_cast<int>(to)] = pl.size();
    });

    const std::uint8_t payload[] = {0xDE, 0xAD, 0xBE, 0xEF};
    auto r = aoi->Broadcast(1, std::span<const std::uint8_t>(payload, 4));
    CHECK(r.HasValue(), "broadcast");
    // 1 视野内：2/3/4（距 ≤50），5 距 ~707 不在视野。故应达 3 个目标。
    CHECK(r.Value() == 3, "broadcast reached exactly visible set (2/3/4)");
    CHECK(recv_count[2] == 1 && recv_count[3] == 1 && recv_count[4] == 1,
          "each visible target received once");
    CHECK(recv_count[5] == 0, "far target received nothing");
    CHECK(payload_seen[2] == 4, "payload delivered intact");
}

}  // namespace

int main() {
    Line("== TASK-014 aoi_test ==\n");
    TestBasicSemantics();
    TestBruteForceConsistency();
    TestMoveDiff();
    TestRandomWalk();
    TestFailures();
    TestBroadcast();

    if (g_fails == 0) {
        Line("ALL PASS\n");
        return 0;
    }
    ErrorFmt("FAILED: %d check(s)\n", g_fails);
    return 1;
}
