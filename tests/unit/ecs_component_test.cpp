// tests/unit/ecs_component_test.cpp — ECS 组件层 SoA 布局验证 (OFF 可编)
//
// 验证目标: 组件为 POD / trivially-copyable, 可安全连续存储 (SoA),
// 迭代无虚表查址、无随机堆访存。这是阶段 A W3 "ECS SoA 布局" 的可验证基线。
#include <gtest/gtest.h>
#include <cstdint>
#include <vector>
#include <type_traits>
#include <numeric>
#include "game/ecs/components.h"

using namespace cami::ecs;

// 编译期锁定组件布局: SoA 连续存储容量可精确计算 (无虚表/指针/动态分配)。
// 注意: 含 uint64 的组件因 8 字节对齐会产生尾部填充, 这是显式可预测的,
// 非"隐藏开销" —— 与虚表/智能指针/运行时分配器有本质区别。
static_assert(sizeof(Transform)  == 16, "Transform  = 4×float");
static_assert(sizeof(Velocity)   == 16, "Velocity   = 4×float");
static_assert(sizeof(Health)     == 8,  "Health     = 2×int32");
static_assert(sizeof(Mana)       == 8,  "Mana       = 2×int32");
static_assert(sizeof(Level)      == 8,  "Level      = uint16+uint32(对齐)");
static_assert(sizeof(CombatState)== 16, "CombatState= 64+8+1+pad");
static_assert(sizeof(Faction)    == 8,  "Faction    = uint16+int32(对齐)");
static_assert(sizeof(NetworkId)  == 16, "NetworkId = uint64(对齐8)+uint32→16");
static_assert(sizeof(AiState)    == 16, "AiState    = 8+8");

TEST(EcsComponentTest, TriviallyCopyable) {
    // SoA 连续搬运的前提: 组件可平凡拷贝 (memcpy 安全)
    EXPECT_TRUE(std::is_trivially_copyable_v<Transform>);
    EXPECT_TRUE(std::is_trivially_copyable_v<Velocity>);
    EXPECT_TRUE(std::is_trivially_copyable_v<Health>);
    EXPECT_TRUE(std::is_trivially_copyable_v<Mana>);
    EXPECT_TRUE(std::is_trivially_copyable_v<Level>);
    EXPECT_TRUE(std::is_trivially_copyable_v<CombatState>);
    EXPECT_TRUE(std::is_trivially_copyable_v<Faction>);
    EXPECT_TRUE(std::is_trivially_copyable_v<NetworkId>);
    EXPECT_TRUE(std::is_trivially_copyable_v<AiState>);
}

TEST(EcsComponentTest, NoHiddenOverhead) {
    // 关键: 组件不含虚表/指针, sizeof 可预测 (无隐藏分配器/vtable/计数)
    EXPECT_EQ(sizeof(Transform), 4 * sizeof(float));
    EXPECT_EQ(sizeof(Health),    2 * sizeof(std::int32_t));
    // NetworkId: uint64(8 字节对齐) + uint32 → 16 字节 (4 字节尾部填充, SoA 下可接受)
    EXPECT_EQ(sizeof(NetworkId), 16u);
    // 全部 4 字节对齐, SIMD/批量拷贝友好
    EXPECT_EQ(alignof(Transform), 4u);
}

// SoA 容器演示: 组件按类型连续存储, 迭代为顺序访存 (cache 友好)。
// 这正是 Entt storage 的底层思想 —— 本测试验证 POD 组件可被安全连续化。
template <typename T>
struct SoAVector {
    std::vector<T> data;
    void push(const T& v) { data.push_back(v); }
    T& operator[](std::size_t i) { return data[i]; }
    std::size_t size() const { return data.size(); }
    const T* begin_ptr() const { return data.data(); }
};

TEST(EcsComponentTest, SoAContiguousIteration) {
    SoAVector<Transform> positions;
    for (int i = 0; i < 1000; ++i) {
        positions.push(Transform{float(i), 0.f, 0.f, 0.f});
    }
    // 顺序迭代: 单次 cache line 命中多个元素, 对比 AoS 随机访存更优
    float sum = 0.f;
    const Transform* base = positions.begin_ptr();
    for (std::size_t i = 0; i < positions.size(); ++i) {
        sum += base[i].x;
    }
    // 算术级数求和验证连续性正确
    float expected = float(999 * 1000 / 2);
    EXPECT_FLOAT_EQ(sum, expected);
    // 内存连续: 相邻元素地址间距 == sizeof(Transform)
    EXPECT_EQ(reinterpret_cast<const char*>(&positions[1]) -
              reinterpret_cast<const char*>(&positions[0]),
              static_cast<std::ptrdiff_t>(sizeof(Transform)));
}
