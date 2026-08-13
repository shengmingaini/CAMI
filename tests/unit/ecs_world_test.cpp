// tests/unit/ecs_world_test.cpp — Entt 封装的 ECS 世界验证 (门控 CAMI_BUILD_MODULES)
//
// 仅在 vcpkg 提供 Entt 时编译 (CI MODULES=ON)。本地 OFF 构建跳过本测试,
// 由 ecs_component_test.cpp 覆盖组件层 SoA 布局。
#include <gtest/gtest.h>
#include "game/ecs/ecs_world.h"

using namespace cami::ecs;

TEST(EcsWorldTest, SpawnPlayerAndIterate) {
    EcsWorld world;
    auto p = world.spawnPlayer(/*playerId=*/1001, /*connId=*/7,
                               /*x=*/10.f, /*y=*/0.f, /*z=*/20.f, /*yaw=*/1.5f,
                               /*maxHp=*/200, /*maxMana=*/150);
    EXPECT_TRUE(world.alive(p));
    EXPECT_EQ(world.get<Transform>(p).x, 10.f);
    EXPECT_EQ(world.get<Health>(p).maxHp, 200);
    EXPECT_EQ(world.get<NetworkId>(p).playerId, 1001u);

    // SoA 友好: 遍历所有带 Transform 的实体 (批量、cache 友好)
    int count = 0;
    world.each<Transform>([&](entt::entity, Transform&) { ++count; });
    EXPECT_EQ(count, 1);
}

TEST(EcsWorldTest, SpawnNpcAndCombatView) {
    EcsWorld world;
    auto a = world.spawnNpc(/*homeCell=*/42, /*behavior=*/2, 5.f, 0.f, 5.f, /*maxHp=*/80);
    auto b = world.spawnPlayer(2002, 9, 6.f, 0.f, 6.f, 0.f, 80, 50);
    EXPECT_EQ(world.alive_count(), 2u);

    // 战斗视图: 同时需要 Transform + Health (交集遍历)
    int combatants = 0;
    world.each<Transform, Health>([&](entt::entity, Transform&, Health&) { ++combatants; });
    EXPECT_EQ(combatants, 2);

    world.destroy(a);
    EXPECT_EQ(world.alive_count(), 1u);
}
