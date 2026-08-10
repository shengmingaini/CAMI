// gateway/connection 单测（周四"单测补齐"交付物）
// 覆盖 connection 模块中**不依赖真实 socket** 的核心路径：
//   - 连接生命周期状态机（所有合法/非法迁移、终态、to_string）
//   - io_context 线程池（size / 轮询分配 / run-stop 可重入）
// socket 拥有类 Connection 与 ConnectionManager 的 acceptor/SO_REUSEPORT 路径
// 属集成测试范畴（需真实网络），由 connection_selfcheck + CI 编译把关，不在此处。
#include <gtest/gtest.h>

#include <boost/asio/io_context.hpp>

#include <vector>

#include "gateway/connection/connection_fsm.h"
#include "gateway/connection/io_context_pool.h"

using cami::gateway::connection::ConnectionState;

// ---------- 状态机：合法迁移 ----------

TEST(ConnectionFsmTest, IdleOnlyToConnecting) {
    EXPECT_TRUE(can_transition(ConnectionState::kIdle, ConnectionState::kConnecting));
    EXPECT_FALSE(can_transition(ConnectionState::kIdle, ConnectionState::kHandshaking));
    EXPECT_FALSE(can_transition(ConnectionState::kIdle, ConnectionState::kEstablished));
    EXPECT_FALSE(can_transition(ConnectionState::kIdle, ConnectionState::kClosing));
    EXPECT_FALSE(can_transition(ConnectionState::kIdle, ConnectionState::kClosed));
    EXPECT_FALSE(can_transition(ConnectionState::kIdle, ConnectionState::kError));
}

TEST(ConnectionFsmTest, ConnectingLegal) {
    EXPECT_TRUE(can_transition(ConnectionState::kConnecting, ConnectionState::kHandshaking));
    EXPECT_TRUE(can_transition(ConnectionState::kConnecting, ConnectionState::kError));
    EXPECT_TRUE(can_transition(ConnectionState::kConnecting, ConnectionState::kClosed));
    EXPECT_FALSE(can_transition(ConnectionState::kConnecting, ConnectionState::kIdle));
    EXPECT_FALSE(can_transition(ConnectionState::kConnecting, ConnectionState::kEstablished));
    EXPECT_FALSE(can_transition(ConnectionState::kConnecting, ConnectionState::kClosing));
}

TEST(ConnectionFsmTest, HandshakingLegal) {
    EXPECT_TRUE(can_transition(ConnectionState::kHandshaking, ConnectionState::kEstablished));
    EXPECT_TRUE(can_transition(ConnectionState::kHandshaking, ConnectionState::kError));
    EXPECT_TRUE(can_transition(ConnectionState::kHandshaking, ConnectionState::kClosed));
    EXPECT_FALSE(can_transition(ConnectionState::kHandshaking, ConnectionState::kIdle));
    EXPECT_FALSE(can_transition(ConnectionState::kHandshaking, ConnectionState::kConnecting));
    EXPECT_FALSE(can_transition(ConnectionState::kHandshaking, ConnectionState::kClosing));
}

TEST(ConnectionFsmTest, EstablishedLegal) {
    EXPECT_TRUE(can_transition(ConnectionState::kEstablished, ConnectionState::kClosing));
    EXPECT_TRUE(can_transition(ConnectionState::kEstablished, ConnectionState::kError));
    EXPECT_TRUE(can_transition(ConnectionState::kEstablished, ConnectionState::kClosed));
    EXPECT_FALSE(can_transition(ConnectionState::kEstablished, ConnectionState::kIdle));
    EXPECT_FALSE(can_transition(ConnectionState::kEstablished, ConnectionState::kConnecting));
    EXPECT_FALSE(can_transition(ConnectionState::kEstablished, ConnectionState::kHandshaking));
}

TEST(ConnectionFsmTest, ClosingLegal) {
    EXPECT_TRUE(can_transition(ConnectionState::kClosing, ConnectionState::kClosed));
    EXPECT_TRUE(can_transition(ConnectionState::kClosing, ConnectionState::kError));
    EXPECT_FALSE(can_transition(ConnectionState::kClosing, ConnectionState::kEstablished));
    EXPECT_FALSE(can_transition(ConnectionState::kClosing, ConnectionState::kIdle));
}

TEST(ConnectionFsmTest, ErrorOnlyToClosed) {
    EXPECT_TRUE(can_transition(ConnectionState::kError, ConnectionState::kClosed));
    EXPECT_FALSE(can_transition(ConnectionState::kError, ConnectionState::kIdle));
    EXPECT_FALSE(can_transition(ConnectionState::kError, ConnectionState::kError));
}

TEST(ConnectionFsmTest, ClosedTerminal) {
    EXPECT_FALSE(can_transition(ConnectionState::kClosed, ConnectionState::kIdle));
    EXPECT_FALSE(can_transition(ConnectionState::kClosed, ConnectionState::kError));
    EXPECT_FALSE(can_transition(ConnectionState::kClosed, ConnectionState::kClosed));
    EXPECT_TRUE(is_terminal(ConnectionState::kClosed));
    EXPECT_FALSE(is_terminal(ConnectionState::kEstablished));
}

// ---------- 状态机：通用不变量 ----------

TEST(ConnectionFsmTest, NoSelfTransition) {
    const ConnectionState all[] = {
        ConnectionState::kIdle,        ConnectionState::kConnecting, ConnectionState::kHandshaking,
        ConnectionState::kEstablished, ConnectionState::kClosing,    ConnectionState::kClosed,
        ConnectionState::kError,
    };
    for (ConnectionState s : all) {
        EXPECT_FALSE(can_transition(s, s)) << "state should not self-transition";
    }
}

TEST(ConnectionFsmTest, ToStringAllStates) {
    EXPECT_STREQ(to_string(ConnectionState::kIdle), "Idle");
    EXPECT_STREQ(to_string(ConnectionState::kConnecting), "Connecting");
    EXPECT_STREQ(to_string(ConnectionState::kHandshaking), "Handshaking");
    EXPECT_STREQ(to_string(ConnectionState::kEstablished), "Established");
    EXPECT_STREQ(to_string(ConnectionState::kClosing), "Closing");
    EXPECT_STREQ(to_string(ConnectionState::kClosed), "Closed");
    EXPECT_STREQ(to_string(ConnectionState::kError), "Error");
}

// ---------- io_context 线程池 ----------

TEST(IoContextPoolTest, SizeReflectsCtor) {
    cami::gateway::connection::IoContextPool pool(4);
    EXPECT_EQ(pool.size(), 4u);
}

TEST(IoContextPoolTest, ZeroSizeDegradesToOne) {
    cami::gateway::connection::IoContextPool pool(0);
    EXPECT_EQ(pool.size(), 1u);
}

TEST(IoContextPoolTest, RoundRobinDistinctAndCycles) {
    cami::gateway::connection::IoContextPool pool(3);
    std::vector<boost::asio::io_context*> seen;
    for (int i = 0; i < 3; ++i) seen.push_back(&pool.get_io_context());
    ASSERT_EQ(seen.size(), 3u);
    EXPECT_NE(seen[0], seen[1]);
    EXPECT_NE(seen[1], seen[2]);
    EXPECT_NE(seen[0], seen[2]);
    // 第 4 次取用应回到第一个（RR 循环）
    EXPECT_EQ(&pool.get_io_context(), seen[0]);
}

TEST(IoContextPoolTest, RunStopIdempotentNoThrow) {
    cami::gateway::connection::IoContextPool pool(2);
    EXPECT_NO_THROW({
        pool.run();
        pool.run();  // 重复调用应为 no-op
        pool.stop();
        pool.stop();  // 重复调用应为 no-op
        pool.run();  // 可再次启停
        pool.stop();
    });
}
