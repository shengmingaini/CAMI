// engine/net/tests/net_test.cpp — TASK-008 §16 单元 / §17 集成 / §19 Failure 测试套件
//
// 覆盖：
//   §16 单元：Buffer 回绕/满/空/延迟分配；ConnectionId 单调/复用/容量耗尽/旧 id 失效
//   §17 集成：echo 往返（Send 自动加帧前缀）、粘包、半包、1MB 大包分片、断连回收
//   §19 失败：端口占用、发送缓冲背压 BUSY、max_connections 拒绝、空闲超时清理
//
// 输出统一走 mmo::core::test（fwrite），禁止裸 std::cout / printf / std::cerr（红线）。
// 依赖前置：TASK-004 engine/core（error/time）。本文件不 include 任何 net 内部 src/。

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <limits>
#include <thread>
#include <vector>
#include <winsock2.h>
#include <ws2tcpip.h>

#include "test_print.h"
#include "mmo/net/buffer.h"
#include "mmo/net/connection.h"
#include "mmo/net/transport.h"

namespace {

using namespace mmo::net;
using mmo::core::test::ErrorFmt;
using mmo::core::test::Line;

int g_fails = 0;

#define CHECK(cond, msg)                                                       \
    do {                                                                       \
        if (!(cond)) {                                                         \
            ErrorFmt("FAIL: %s (line %d)\n", msg, __LINE__);                   \
            ++g_fails;                                                         \
        }                                                                      \
    } while (0)

// ---- 客户端工具（测试专用原始 WinSock，与服务端真实收发） ----
struct TestClient {
    SOCKET fd = INVALID_SOCKET;
    ~TestClient() {
        if (fd != INVALID_SOCKET) {
            closesocket(fd);
        }
    }
    bool Connect(const char* ip, uint16_t port) {
        fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd == INVALID_SOCKET) {
            return false;
        }
        sockaddr_in sin{};
        sin.sin_family = AF_INET;
        sin.sin_port = htons(port);
        inet_pton(AF_INET, ip, &sin.sin_addr);
        u_long mode = 1;
        ioctlsocket(fd, FIONBIO, &mode);
        connect(fd, reinterpret_cast<const sockaddr*>(&sin), sizeof(sin));
        fd_set w;
        FD_ZERO(&w);
        FD_SET(fd, &w);
        timeval tv{5, 0};
        return select(0, nullptr, &w, nullptr, &tv) == 1;
    }
    bool SendAll(const uint8_t* data, int len) {
        int sent = 0;
        while (sent < len) {
            const int n = send(fd, reinterpret_cast<const char*>(data) + sent, len - sent, 0);
            if (n > 0) {
                sent += n;
                continue;
            }
            if (n == SOCKET_ERROR && WSAGetLastError() == WSAEWOULDBLOCK) {
                // 非阻塞发送遇内核缓冲满：等待可写后重试（大包分片场景必现）
                fd_set w;
                FD_ZERO(&w);
                FD_SET(fd, &w);
                timeval tv{1, 0};
                select(0, nullptr, &w, nullptr, &tv);
                continue;
            }
            return false;
        }
        return true;
    }
    int RecvSome(uint8_t* buf, int cap, int timeout_ms = 3000) {
        fd_set r;
        FD_ZERO(&r);
        FD_SET(fd, &r);
        timeval tv{timeout_ms / 1000, (timeout_ms % 1000) * 1000};
        if (select(0, &r, nullptr, nullptr, &tv) != 1) {
            return -2;  // 超时
        }
        return recv(fd, reinterpret_cast<char*>(buf), cap, 0);
    }
};

// 打包：4B 大端长度前缀 + payload（与服务端帧格式一致）
std::vector<uint8_t> Pack(const uint8_t* payload, uint32_t len) {
    std::vector<uint8_t> out(4 + len);
    out[0] = static_cast<uint8_t>(len >> 24);
    out[1] = static_cast<uint8_t>(len >> 16);
    out[2] = static_cast<uint8_t>(len >> 8);
    out[3] = static_cast<uint8_t>(len);
    if (len > 0) {
        std::memcpy(out.data() + 4, payload, len);
    }
    return out;
}

// 服务端 Poll 直到出现指定事件（最多 rounds 轮）
bool PollUntil(std::unique_ptr<INetworkTransport>& t, TransportEvent::Kind k,
               std::vector<TransportEvent>& collected, int rounds = 60) {
    std::vector<TransportEvent> out;
    for (int i = 0; i < rounds; ++i) {
        out.clear();
        (void)t->Poll(std::chrono::milliseconds(20), out);
        for (auto& ev : out) {
            collected.push_back(ev);
            if (ev.kind == k) {
                return true;
            }
        }
    }
    return false;
}

// ===========================================================================
// §16 单元测试
// ===========================================================================

void TestBuffer() {
    // 回绕：容量 8，写 5 读 3，再写 5 触发回绕
    Buffer b(8);
    CHECK(b.Size() == 0 && b.Free() == 8, "buffer initial state");
    CHECK(!b.Allocated(), "buffer lazy allocation (0 bytes until first write)");
    const uint8_t a[5] = {1, 2, 3, 4, 5};
    CHECK(b.Write(a), "write 5 bytes");
    CHECK(b.Allocated(), "buffer allocated after first write");
    uint8_t dst[3];
    const size_t rd = b.Read(dst);  // 数组 -> span 隐式转换
    (void)rd;
    CHECK(rd == 3 && dst[0] == 1 && dst[2] == 3, "read 3 bytes");
    const uint8_t bb[5] = {6, 7, 8, 9, 10};
    CHECK(b.Write(bb), "write 5 more (wrap)");
    uint8_t all[7];
    CHECK(b.Read(all) == 7, "read all 7");
    CHECK(all[0] == 4 && all[3] == 7 && all[6] == 10, "wrap content correct");
    CHECK(b.Empty(), "buffer empty after drain");

    // 满 -> BUSY（全有或全无）
    Buffer small(4);
    const uint8_t f4[4] = {1, 2, 3, 4};
    CHECK(small.Write(f4), "fill 4");
    const uint8_t more[2] = {5, 6};
    auto r = small.Write(more);
    CHECK(!r && r.Err().Code() == mmo::core::ErrorCode::BUSY, "write full -> BUSY");
    CHECK(small.Size() == 4, "no partial write on BUSY");
    small.Reset();
    CHECK(small.Size() == 0 && !small.Allocated(), "Reset releases memory");
}

void TestAllocator() {
    ConnectionIdAllocator alloc(4);
    CHECK(alloc.Capacity() == 4, "allocator capacity");
    auto id1 = alloc.Allocate();
    auto id2 = alloc.Allocate();
    CHECK(id1.has_value() && id2.has_value(), "allocate 2 ids");
    CHECK(*id2 > *id1, "ids monotonically increasing");
    CHECK(alloc.IsValid(*id1) && alloc.IsValid(*id2), "ids valid while in use");
    alloc.Release(*id1);
    CHECK(!alloc.IsValid(*id1), "released id invalid");
    CHECK(alloc.Size() == 1, "size after release");
    auto id3 = alloc.Allocate();
    CHECK(id3.has_value() && *id3 > *id2, "slot reuse keeps monotonic serial");
    CHECK(alloc.IsValid(*id3), "new id valid");
    // 容量耗尽
    ConnectionIdAllocator tiny(1);
    auto t1 = tiny.Allocate();
    CHECK(t1.has_value(), "tiny allocate 1");
    CHECK(!tiny.Allocate().has_value(), "tiny capacity exhausted");
    (void)t1;
    // 垃圾 id 安全
    CHECK(!alloc.IsValid(0), "id 0 invalid");
    CHECK(!alloc.IsValid(std::numeric_limits<ConnectionId>::max()), "max id invalid");
}

// ===========================================================================
// §17 集成测试
// ===========================================================================

void TestEchoRoundTrip() {
    TcpConfig cfg;
    cfg.max_connections = 16;
    cfg.recv_buf = 8192;
    cfg.send_buf = 8192;
    cfg.keepalive_idle_s = 0;
    auto t = CreateTcpTransport(cfg);
    CHECK(t != nullptr, "create transport");
    auto lr = t->Listen("127.0.0.1", 39321);
    CHECK(lr, "listen 39321");
    if (!lr) {
        return;
    }
    TestClient c;
    CHECK(c.Connect("127.0.0.1", 39321), "client connect");

    std::vector<TransportEvent> events;
    CHECK(PollUntil(t, TransportEvent::Kind::Connected, events), "server sees Connected");
    CHECK(t->ConnectionCount() == 1, "conn count == 1");
    ConnectionId cid = 0;
    for (auto& ev : events) {
        if (ev.kind == TransportEvent::Kind::Connected) {
            cid = ev.conn_id;
        }
    }
    CHECK(cid != 0, "valid conn id");
    IConnection* conn = t->Get(cid);
    CHECK(conn != nullptr, "Get(conn_id) non-null");
    CHECK(t->Get(std::numeric_limits<ConnectionId>::max()) == nullptr, "Get(invalid id) null");

    // 客户端 -> 服务端（Recv 方向）
    const uint8_t hello[] = {'h', 'e', 'l', 'l', 'o'};
    auto pkt = Pack(hello, 5);
    CHECK(c.SendAll(pkt.data(), static_cast<int>(pkt.size())), "client send hello");
    events.clear();
    CHECK(PollUntil(t, TransportEvent::Kind::Received, events), "server sees Received");
    bool got_hello = false;
    for (auto& ev : events) {
        if (ev.kind == TransportEvent::Kind::Received && ev.conn_id == cid) {
            got_hello = (ev.data.size() == 5 && ev.data[0] == 'h' && ev.data[4] == 'o');
        }
    }
    CHECK(got_hello, "hello payload matches (prefix stripped)");

    // 服务端 -> 客户端（Send 方向，自动加帧前缀）
    const uint8_t world[] = {'w', 'o', 'r', 'l', 'd'};
    CHECK(conn->Send(world), "server Send(world)");
    bool saw_drained = false;
    for (int round = 0; round < 5 && !saw_drained; ++round) {
        events.clear();
        (void)t->Poll(std::chrono::milliseconds(20), events);
        for (auto& ev : events) {
            if (ev.kind == TransportEvent::Kind::SendDrained && ev.conn_id == cid) {
                saw_drained = true;
            }
        }
    }
    CHECK(saw_drained, "server emits SendDrained after drain");
    uint8_t hdr[4];
    const int hn = c.RecvSome(hdr, 4, 3000);
    CHECK(hn == 4 && hdr[0] == 0 && hdr[1] == 0 && hdr[2] == 0 && hdr[3] == 5,
          "client recv length prefix (5)");
    uint8_t body[5];
    int bn = 0;
    while (bn < 5) {
        const int n = c.RecvSome(body + bn, 5 - bn, 3000);
        if (n <= 0) {
            break;
        }
        bn += n;
    }
    CHECK(bn == 5 && body[0] == 'w' && body[4] == 'd', "client recv payload world");

    // 优雅关闭：服务端 Close -> FIN，客户端 recv == 0
    (void)conn->Close(CloseReason::Graceful);
    bool saw_disc = false;
    for (int round = 0; round < 10 && !saw_disc; ++round) {
        events.clear();
        (void)t->Poll(std::chrono::milliseconds(20), events);
        for (auto& ev : events) {
            if (ev.kind == TransportEvent::Kind::Disconnected && ev.conn_id == cid) {
                saw_disc = true;
            }
        }
    }
    CHECK(saw_disc, "server emits Disconnected after graceful close");
    CHECK(t->ConnectionCount() == 0, "conn count back to 0");
    uint8_t junk[16];
    CHECK(c.RecvSome(junk, sizeof(junk), 3000) == 0, "client sees FIN");
}

void TestStickyHalfPackets() {
    TcpConfig cfg;
    cfg.max_connections = 16;
    cfg.recv_buf = 8192;
    cfg.send_buf = 8192;
    cfg.keepalive_idle_s = 0;
    auto t = CreateTcpTransport(cfg);
    CHECK(t && t->Listen("127.0.0.1", 39322), "listen 39322");
    TestClient c;
    CHECK(c.Connect("127.0.0.1", 39322), "connect 39322");
    std::vector<TransportEvent> events;
    CHECK(PollUntil(t, TransportEvent::Kind::Connected, events), "connected 39322");

    // 粘包：一次发 3 个包
    const uint8_t a[] = {'A'};
    const uint8_t b[] = {'B', 'B'};
    const uint8_t cc[] = {'C', 'C', 'C'};
    auto pa = Pack(a, 1);
    auto pb = Pack(b, 2);
    auto pc = Pack(cc, 3);
    std::vector<uint8_t> blob;
    blob.insert(blob.end(), pa.begin(), pa.end());
    blob.insert(blob.end(), pb.begin(), pb.end());
    blob.insert(blob.end(), pc.begin(), pc.end());
    CHECK(c.SendAll(blob.data(), static_cast<int>(blob.size())), "send 3 packets at once");

    int got = 0;
    bool contents_ok = true;
    for (int round = 0; round < 20 && got < 3; ++round) {
        events.clear();
        (void)t->Poll(std::chrono::milliseconds(20), events);
        for (auto& ev : events) {
            if (ev.kind == TransportEvent::Kind::Received) {
                if (got == 0) {
                    contents_ok &= (ev.data.size() == 1 && ev.data[0] == 'A');
                }
                if (got == 1) {
                    contents_ok &= (ev.data.size() == 2 && ev.data[0] == 'B');
                }
                if (got == 2) {
                    contents_ok &= (ev.data.size() == 3 && ev.data[0] == 'C');
                }
                ++got;
            }
        }
    }
    CHECK(got == 3, "3 packets parsed from sticky blob (single Poll may emit multiple)");
    CHECK(contents_ok, "sticky packet contents correct");

    // 半包：14 字节的帧分两次发（7 + 7）
    const uint8_t big[10] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
    auto pbig = Pack(big, 10);
    CHECK(c.SendAll(pbig.data(), 7), "send half packet (7 of 14)");
    // 中途 Poll 不应 emit（半包保留在缓冲）
    events.clear();
    (void)t->Poll(std::chrono::milliseconds(50), events);
    bool early_emit = false;
    for (auto& ev : events) {
        if (ev.kind == TransportEvent::Kind::Received) {
            early_emit = true;
        }
    }
    CHECK(!early_emit, "no emit on half packet");
    CHECK(c.SendAll(pbig.data() + 7, static_cast<int>(pbig.size()) - 7), "send rest");
    events.clear();
    bool got_big = false;
    for (int round = 0; round < 10 && !got_big; ++round) {
        events.clear();
        (void)t->Poll(std::chrono::milliseconds(20), events);
        for (auto& ev : events) {
            if (ev.kind == TransportEvent::Kind::Received) {
                got_big = (ev.data.size() == 10 && ev.data[9] == 10);
            }
        }
    }
    CHECK(got_big, "half packet reassembled after second send");
}

void TestBigPacket1MB() {
    TcpConfig cfg;
    cfg.max_connections = 16;
    // 大包需要 recv_buf 足以容纳完整帧（kMaxPayloadBytes = 1 MiB + 4B 前缀 + 余量）
    cfg.recv_buf = kMaxPayloadBytes + kLengthPrefixBytes + 64 * 1024;
    cfg.send_buf = 8192;
    cfg.keepalive_idle_s = 0;
    auto t = CreateTcpTransport(cfg);
    CHECK(t && t->Listen("127.0.0.1", 39323), "listen 39323");
    TestClient c;
    CHECK(c.Connect("127.0.0.1", 39323), "connect 39323");
    std::vector<TransportEvent> events;
    CHECK(PollUntil(t, TransportEvent::Kind::Connected, events), "connected 39323");

    // 构造 1MB payload（首尾哨兵 + 填充模式），分 4 次发送模拟分片
    const uint32_t kPayload = 1 << 20;
    std::vector<uint8_t> payload(kPayload);
    for (uint32_t i = 0; i < kPayload; ++i) {
        payload[i] = static_cast<uint8_t>((i * 31 + 7) & 0xFF);
    }
    auto frame = Pack(payload.data(), kPayload);
    // 大包发送必然触发客户端 would-block（内核缓冲有限），服务端必须并发消费：
    // 起独立 IO 线程跑 Poll（真实线程模型），主线程负责发送。
    std::atomic<bool> got_1mb{false};
    std::thread io_thread([&] {
        std::vector<TransportEvent> out;
        for (int round = 0; round < 200 && !got_1mb.load(std::memory_order_relaxed); ++round) {
            out.clear();
            (void)t->Poll(std::chrono::milliseconds(20), out);
            for (auto& ev : out) {
                if (ev.kind == TransportEvent::Kind::Received &&
                    ev.data.size() == kPayload) {
                    got_1mb.store(
                        ev.data[0] == payload[0] &&
                            ev.data[kPayload - 1] == payload[kPayload - 1] &&
                            ev.data[kPayload / 2] == payload[kPayload / 2],
                        std::memory_order_relaxed);
                }
            }
        }
    });
    const int chunk = static_cast<int>(frame.size()) / 4;
    for (int off = 0; off < static_cast<int>(frame.size()); off += chunk) {
        const int n = std::min(chunk, static_cast<int>(frame.size()) - off);
        CHECK(c.SendAll(frame.data() + off, n), "send 1MB frame chunk");
    }
    io_thread.join();
    CHECK(got_1mb.load(), "1MB packet received intact");
}

void TestDisconnectRecycle() {
    TcpConfig cfg;
    cfg.max_connections = 16;
    cfg.recv_buf = 8192;
    cfg.send_buf = 8192;
    cfg.keepalive_idle_s = 0;
    auto t = CreateTcpTransport(cfg);
    CHECK(t && t->Listen("127.0.0.1", 39324), "listen 39324");
    {
        TestClient c;
        CHECK(c.Connect("127.0.0.1", 39324), "connect 39324");
        std::vector<TransportEvent> events;
        CHECK(PollUntil(t, TransportEvent::Kind::Connected, events), "connected 39324");
        CHECK(t->ConnectionCount() == 1, "count 1");
    }  // 析构 -> 对端关闭
    std::vector<TransportEvent> events;
    bool saw_disconn = false;
    for (int round = 0; round < 20 && !saw_disconn; ++round) {
        events.clear();
        (void)t->Poll(std::chrono::milliseconds(20), events);
        for (auto& ev : events) {
            if (ev.kind == TransportEvent::Kind::Disconnected) {
                saw_disconn = true;
            }
        }
    }
    CHECK(saw_disconn, "server sees Disconnected after peer close");
    CHECK(t->ConnectionCount() == 0, "conn count back to 0");
}

// ===========================================================================
// §19 Failure 测试
// ===========================================================================

void TestPortInUse() {
    TcpConfig cfg;
    cfg.max_connections = 16;
    auto t1 = CreateTcpTransport(cfg);
    auto t2 = CreateTcpTransport(cfg);
    CHECK(t1->Listen("127.0.0.1", 39325), "first listen ok");
    auto r = t2->Listen("127.0.0.1", 39325);
    CHECK(!r, "second listen returns error (port in use)");
    if (!r) {
        CHECK(r.Err().Code() == mmo::core::ErrorCode::INVALID_ARGUMENT,
              "port-in-use error code INVALID_ARGUMENT");
    }
}

void TestSendBackpressure() {
    TcpConfig cfg;
    cfg.max_connections = 16;
    cfg.recv_buf = 8192;
    cfg.send_buf = 4096;  // 小发送缓冲：触发背压
    cfg.keepalive_idle_s = 0;
    auto t = CreateTcpTransport(cfg);
    CHECK(t && t->Listen("127.0.0.1", 39326), "listen 39326");
    TestClient c;  // 客户端不读 -> 内核发送缓冲很快打满
    CHECK(c.Connect("127.0.0.1", 39326), "connect 39326");
    std::vector<TransportEvent> events;
    CHECK(PollUntil(t, TransportEvent::Kind::Connected, events), "connected 39326");
    ConnectionId cid = 0;
    for (auto& ev : events) {
        if (ev.kind == TransportEvent::Kind::Connected) {
            cid = ev.conn_id;
        }
    }
    IConnection* conn = t->Get(cid);
    CHECK(conn != nullptr, "Get non-null");

    // 2KB × 2 = 4008B 帧（含 4B 前缀）< 4096 可入缓冲；第 3 帧超出 -> BUSY
    std::vector<uint8_t> block(2000, 0x5A);
    CHECK(conn->Send(block), "send #1 ok");
    CHECK(conn->Send(block), "send #2 ok");
    auto r3 = conn->Send(block);
    CHECK(!r3 && r3.Err().Code() == mmo::core::ErrorCode::BUSY, "send #3 -> BUSY");
    // 发送队列深度应反映未排空字节（>0，未发生无界增长）
    const auto st = t->Stats();
    CHECK(st.send_queue_depth >= 2000 * 2, "send queue depth reflects pending bytes");
}

void TestMaxConnReject() {
    TcpConfig cfg;
    cfg.max_connections = 2;
    cfg.recv_buf = 8192;
    cfg.send_buf = 8192;
    cfg.keepalive_idle_s = 0;
    auto t = CreateTcpTransport(cfg);
    CHECK(t && t->Listen("127.0.0.1", 39327), "listen 39327");
    TestClient c1, c2, c3;
    CHECK(c1.Connect("127.0.0.1", 39327), "c1 connect");
    CHECK(c2.Connect("127.0.0.1", 39327), "c2 connect");
    std::vector<TransportEvent> events;
    CHECK(PollUntil(t, TransportEvent::Kind::Connected, events), "2 connected");
    CHECK(t->ConnectionCount() == 2, "count == 2");
    c3.Connect("127.0.0.1", 39327);  // 第 3 个被拒绝
    // 服务端必须再 Poll 一轮 accept 到 c3 才会计数拒绝
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    std::vector<TransportEvent> tmp;
    for (int round = 0; round < 5; ++round) {
        (void)t->Poll(std::chrono::milliseconds(20), tmp);
    }
    CHECK(t->ConnectionCount() == 2, "still 2 after reject");
    CHECK(t->Stats().error_count >= 1, "reject counted in error_count");
}

void TestIdleTimeout() {
    TcpConfig cfg;
    cfg.max_connections = 16;
    cfg.recv_buf = 8192;
    cfg.send_buf = 8192;
    cfg.keepalive_idle_s = 1;  // 1 秒空闲即超时
    auto t = CreateTcpTransport(cfg);
    CHECK(t && t->Listen("127.0.0.1", 39328), "listen 39328");
    TestClient c;  // 只连不发 -> 半开连接
    CHECK(c.Connect("127.0.0.1", 39328), "connect 39328");
    std::vector<TransportEvent> events;
    CHECK(PollUntil(t, TransportEvent::Kind::Connected, events), "connected 39328");
    CHECK(t->ConnectionCount() == 1, "count 1");

    bool saw_timeout = false;
    bool saw_conn_count_zero = false;
    for (int round = 0; round < 100 && !saw_timeout; ++round) {
        events.clear();
        (void)t->Poll(std::chrono::milliseconds(50), events);
        for (auto& ev : events) {
            if (ev.kind == TransportEvent::Kind::Disconnected &&
                ev.error.Code() == mmo::core::ErrorCode::TIMEOUT) {
                saw_timeout = true;
            }
        }
    }
    CHECK(saw_timeout, "idle timeout emits Disconnected(TIMEOUT)");
    CHECK(t->ConnectionCount() == 0, "conn count 0 after timeout");
    (void)saw_conn_count_zero;
}

void TestOversizedPacketReject() {
    // 长度前缀 > kMaxPayloadBytes：服务端必须拒绝并断开（§16 超大包拒绝）
    TcpConfig cfg;
    cfg.max_connections = 16;
    cfg.recv_buf = 8192;
    cfg.send_buf = 8192;
    cfg.keepalive_idle_s = 0;
    auto t = CreateTcpTransport(cfg);
    CHECK(t && t->Listen("127.0.0.1", 39329), "listen 39329");
    TestClient c;
    CHECK(c.Connect("127.0.0.1", 39329), "connect 39329");
    std::vector<TransportEvent> events;
    CHECK(PollUntil(t, TransportEvent::Kind::Connected, events), "connected 39329");
    ConnectionId cid = 0;
    for (auto& ev : events) {
        if (ev.kind == TransportEvent::Kind::Connected) {
            cid = ev.conn_id;
        }
    }

    // 声明长度 = 1MB + 1（超限），只发前缀 + 1 字节 payload
    const uint32_t bad_len = kMaxPayloadBytes + 1;
    uint8_t hdr[4] = {static_cast<uint8_t>(bad_len >> 24),
                      static_cast<uint8_t>(bad_len >> 16),
                      static_cast<uint8_t>(bad_len >> 8),
                      static_cast<uint8_t>(bad_len)};
    uint8_t junk = 0x01;
    CHECK(c.SendAll(hdr, 4), "send oversized length prefix");
    CHECK(c.SendAll(&junk, 1), "send 1 payload byte");

    bool saw_disc = false;
    for (int round = 0; round < 10 && !saw_disc; ++round) {
        events.clear();
        (void)t->Poll(std::chrono::milliseconds(20), events);
        for (auto& ev : events) {
            if (ev.kind == TransportEvent::Kind::Disconnected && ev.conn_id == cid) {
                saw_disc = true;
            }
        }
    }
    CHECK(saw_disc, "oversized packet -> server disconnects");
    CHECK(t->ConnectionCount() == 0, "conn count 0 after reject");
}

void TestAbruptDisconnectRST() {
    // 客户端 SO_LINGER(0) + close -> RST：服务端感知并回收（§19 异常断连）
    TcpConfig cfg;
    cfg.max_connections = 16;
    cfg.recv_buf = 8192;
    cfg.send_buf = 8192;
    cfg.keepalive_idle_s = 0;
    auto t = CreateTcpTransport(cfg);
    CHECK(t && t->Listen("127.0.0.1", 39330), "listen 39330");
    std::vector<TransportEvent> events;
    {
        TestClient c;
        CHECK(c.Connect("127.0.0.1", 39330), "connect 39330");
        CHECK(PollUntil(t, TransportEvent::Kind::Connected, events), "connected 39330");
        CHECK(t->ConnectionCount() == 1, "count 1");
        // 设置 SO_LINGER{on=1, timeout=0} 后关闭 -> 立即 RST（丢弃未发送数据）
        linger lg{};
        lg.l_onoff = 1;
        lg.l_linger = 0;
        setsockopt(c.fd, SOL_SOCKET, SO_LINGER, reinterpret_cast<const char*>(&lg), sizeof(lg));
    }  // 析构 closesocket -> RST
    bool saw_disc = false;
    for (int round = 0; round < 20 && !saw_disc; ++round) {
        events.clear();
        (void)t->Poll(std::chrono::milliseconds(20), events);
        for (auto& ev : events) {
            if (ev.kind == TransportEvent::Kind::Disconnected) {
                saw_disc = true;
            }
        }
    }
    CHECK(saw_disc, "RST -> server emits Disconnected");
    CHECK(t->ConnectionCount() == 0, "conn count 0 after RST");
}

}  // namespace

int main() {
    WSADATA wsa{};
    WSAStartup(MAKEWORD(2, 2), &wsa);

    TestBuffer();
    TestAllocator();
    TestEchoRoundTrip();
    TestStickyHalfPackets();
    TestBigPacket1MB();
    TestDisconnectRecycle();
    TestPortInUse();
    TestSendBackpressure();
    TestMaxConnReject();
    TestIdleTimeout();
    TestOversizedPacketReject();
    TestAbruptDisconnectRST();

    if (g_fails == 0) {
        Line("NET_TEST_OK\n");
        return 0;
    }
    ErrorFmt("NET_TEST_FAIL fails=%d\n", g_fails);
    return 1;
}
