#include "gateway/connection/connection_selfcheck.h"
#include "gateway/connection/connection_fsm.h"
#include "gateway/connection/io_context_pool.h"

#include <chrono>
#include <cstdio>
#include <thread>

namespace cami {
namespace gateway {
namespace connection {

bool connection_selfcheck() {
    bool ok = true;

    // 1) FSM 合法迁移
    if (!can_transition(ConnectionState::kIdle, ConnectionState::kConnecting)) {
        std::fprintf(stderr, "[FAIL] FSM: Idle->Connecting 应合法\n"); ok = false;
    }
    if (!can_transition(ConnectionState::kEstablished, ConnectionState::kClosing)) {
        std::fprintf(stderr, "[FAIL] FSM: Established->Closing 应合法\n"); ok = false;
    }
    // 2) FSM 非法迁移（应被拒绝，禁止静默）
    if (can_transition(ConnectionState::kClosed, ConnectionState::kEstablished)) {
        std::fprintf(stderr, "[FAIL] FSM: Closed->Established 应非法\n"); ok = false;
    }
    if (can_transition(ConnectionState::kIdle, ConnectionState::kClosed)) {
        std::fprintf(stderr, "[FAIL] FSM: Idle->Closed 应非法(须经 Connecting)\n"); ok = false;
    }

    // 3) IoContextPool 启停 + 轮询分配（不崩溃即视为通过；不绑端口）
    IoContextPool pool(4);
    if (pool.size() != 4) {
        std::fprintf(stderr, "[FAIL] pool size 应为 4\n"); ok = false;
    }
    pool.run();
    for (int i = 0; i < 8; ++i) {
        (void)pool.get_io_context();  // 覆盖全部 4 个 io_context（轮询）
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));  // 验证 run() 真拉起线程
    pool.stop();

    // 4) 退化场景：pool_size=0 → 1
    IoContextPool tiny(0);
    if (tiny.size() != 1) {
        std::fprintf(stderr, "[FAIL] pool_size=0 应退化为 1\n"); ok = false;
    }

    return ok;
}

}  // namespace connection
}  // namespace gateway
}  // namespace cami
