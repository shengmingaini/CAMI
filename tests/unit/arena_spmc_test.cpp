#include <gtest/gtest.h>
#include <atomic>
#include <cstring>
#include <string>
#include <thread>
#include <vector>
#include "common/arena.h"
#include "common/spmc_queue.h"

using namespace cami::common;

// ============================ Arena ============================

TEST(ArenaTest, BasicAllocation) {
    Arena a;
    void* p1 = a.Allocate(16);
    void* p2 = a.Allocate(32);
    EXPECT_NE(p1, nullptr);
    EXPECT_NE(p2, nullptr);
    EXPECT_NE(p1, p2);
    EXPECT_EQ(a.Allocated(), 48u);
    a.Reset();
    EXPECT_EQ(a.Allocated(), 0u);
}

TEST(ArenaTest, Alignment8Bytes) {
    Arena a;
    void* p = a.Allocate(1);
    EXPECT_EQ(reinterpret_cast<std::uintptr_t>(p) % 8, 0u);  // 8 字节对齐
}

TEST(ArenaTest, CrossPage) {
    Arena a(64);  // 小页 64B, 分配 50B 实际对齐到 56B
    void* p1 = a.Allocate(50);
    EXPECT_EQ(a.PageCount(), 1u);
    void* p2 = a.Allocate(50);  // 56+56=112 > 64, 触发新页
    EXPECT_EQ(a.PageCount(), 2u);
    EXPECT_NE(p1, p2);
    EXPECT_EQ(a.Allocated(), 112u);  // 50→56 对齐, ×2
}

TEST(ArenaTest, ResetReuses) {
    Arena a(64);
    a.Allocate(60);
    a.Reset();
    EXPECT_EQ(a.PageCount(), 1u);  // 页保留
    EXPECT_EQ(a.Allocated(), 0u);
    void* p = a.Allocate(30);      // 复用游标
    EXPECT_EQ(a.Allocated(), 32u); // 对齐到 8 字节
    (void)p;
}

TEST(ArenaTest, StringViaArena) {
    Arena a(256);
    // 模拟请求处理: 构造临时字符串到 Arena
    char* buf = static_cast<char*>(a.Allocate(32));
    std::memcpy(buf, "hello arena", 12);
    EXPECT_STREQ(buf, "hello arena");
    const char* before = buf;
    a.Reset();
    // Reset 后内存仍可用 (页面未释放), Reset 后立即复用
    char* buf2 = static_cast<char*>(a.Allocate(32));
    EXPECT_EQ(buf2, before);  // 复用同一块
}

// ============================ SPMCQueue ============================

TEST(SPMCQueueTest, PushPopSingleThread) {
    SPMCQueue<int, 8> q;
    for (int i = 0; i < 4; ++i) {
        EXPECT_TRUE(q.TryPush(i));
    }
    for (int i = 0; i < 4; ++i) {
        int v;
        EXPECT_TRUE(q.TryPop(v));
        EXPECT_EQ(v, i);
    }
}

TEST(SPMCQueueTest, FullEmpty) {
    SPMCQueue<int, 4> q;  // 有效容量 3
    EXPECT_TRUE(q.TryPush(1));
    EXPECT_TRUE(q.TryPush(2));
    EXPECT_TRUE(q.TryPush(3));
    EXPECT_FALSE(q.TryPush(4));  // 满
    int v;
    EXPECT_TRUE(q.TryPop(v)); EXPECT_EQ(v, 1);
    EXPECT_TRUE(q.TryPop(v)); EXPECT_EQ(v, 2);
    EXPECT_TRUE(q.TryPop(v)); EXPECT_EQ(v, 3);
    EXPECT_FALSE(q.TryPop(v));  // 空
}

TEST(SPMCQueueTest, MultiConsumer) {
    SPMCQueue<int, 64> q;
    for (int i = 0; i < 40; ++i) q.TryPush(i);

    std::atomic<int> sum{0};
    std::atomic<int> done{0};
    constexpr int kThreads = 3;
    std::vector<std::thread> ts;
    for (int t = 0; t < kThreads; ++t) {
        ts.emplace_back([&] {
            int v;
            while (q.TryPop(v)) {
                sum.fetch_add(v, std::memory_order_relaxed);
            }
            done.fetch_add(1, std::memory_order_relaxed);
        });
    }
    for (auto& t : ts) t.join();
    // 三个线程均完成; 和 = 0+1+...+39 = 780
    EXPECT_EQ(done.load(), kThreads);
    EXPECT_EQ(sum.load(), 780);
}

TEST(SPMCQueueTest, CapacityIsPowerOfTwoMinusOne) {
    SPMCQueue<long, 16> q;
    EXPECT_EQ(q.Capacity(), 15u);  // 1 槽用于区分空/满
}
