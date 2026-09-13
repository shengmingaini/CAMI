// client/core/tests/client_test.cpp — TASK-034 Client Core 单元测试
//
// 覆盖任务书验收口径：
//   §12 帧精度（固定 60Hz 逻辑节拍 P95）
//   §13 客户端世界镜像 + 插值（缓冲 / 外推上限冻结）
//   §15 输入采样（按下 / 边沿 / 采样后清空）
//   §18 协议往返（NetClient <-> MockServer 真实 TCP + 真实 Envelope 编解码）
//   §19 断网重连（DropClient 触发自动重连；服务端关闭转入 Disconnected）
//   §20 配置加载（JSON 解析 + 缺省回退）

#include "mmo/client/client_world.h"
#include "mmo/client/config.h"
#include "mmo/client/game_loop.h"
#include "mmo/client/input.h"
#include "mmo/client/net_client.h"

#include "mock_server.h"
#include "test_print.h"

#include "mmo/protocol/codec/flatbuf_codec.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

namespace tprint = ::mmo::core::test;
using namespace mmo::client;
namespace core = mmo::core;
namespace protocol = mmo::protocol;

int g_passed = 0;
int g_failed = 0;

#define CHECK(cond, name)                                                     \
    do {                                                                     \
        if (cond) {                                                          \
            ++g_passed;                                                      \
            tprint::LineFmt("[PASS] %s\n", name);                            \
        } else {                                                             \
            ++g_failed;                                                      \
            tprint::LineFmt("[FAIL] %s (line %d)\n", name, __LINE__);        \
        }                                                                    \
    } while (0)

// 轮询等待并取一个事件（最多 ~wait_ms）
static bool DrainEvent(NetClient& cli, std::vector<std::uint8_t>& out, int wait_ms) {
    for (int t = 0; t < wait_ms / 10; ++t) {
        if (cli.TryRecvEvent(out)) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return false;
}

// 带重试的请求（容忍重连窗口）
static bool RequestRetry(NetClient& cli, const std::string& payload, int tries = 20) {
    for (int i = 0; i < tries; ++i) {
        auto r = cli.Request(payload);
        if (r.HasValue()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return false;
}

int main() {
    // §12 帧精度：固定 60Hz，跑 1s，tick 节拍 P95 应接近 16.67ms（容差 30ms）
    {
        GameLoopConfig cfg;  // 默认 60Hz
        auto stats = GameLoop::Run(cfg, 1000, [](core::SteadyNs, core::SteadyNs) {});
        CHECK(stats.ticks >= 50, "T12_ticks_ge_50");
        CHECK(stats.tick_ms_p95 > 0.0 && stats.tick_ms_p95 <= 30.0, "T12_tick_p95_le_30");
        CHECK(stats.catchups == 0, "T12_no_catchup");
        tprint::LineFmt("  [info] ticks=%llu tick_p95_ms=%.3f frames=%llu\n",
                        (unsigned long long)stats.ticks, stats.tick_ms_p95,
                        (unsigned long long)stats.frames);
    }

    // §13 插值：两段快照 @ ts=0/100，render @ 50 -> 中点；@ 350 -> 超外推冻结
    {
        ClientWorld w;
        WorldSnapshot s1; s1.server_time_ms = 0;
        s1.entities.push_back(EntityPose{1, Vec3{0,0,0}, {}, 0.0f, 0});
        WorldSnapshot s2; s2.server_time_ms = 100;
        s2.entities.push_back(EntityPose{1, Vec3{10,0,0}, {}, 0.0f, 100});
        w.ApplySnapshot(s1);
        w.ApplySnapshot(s2);
        auto r1 = w.Interpolate(50);
        CHECK(r1.size() == 1 && std::fabs(r1[0].pos.x - 5.0f) < 0.5f, "T13_interp_midpoint");
        auto r2 = w.Interpolate(350);  // 100 + 200 外推上限 < 350 -> 冻结
        CHECK(r2.size() == 1 && r2[0].frozen, "T13_extrapolation_freeze");
        // 单快照：直接取当前姿态
        ClientWorld w1;
        w1.ApplySnapshot(s1);
        auto r3 = w1.Interpolate(0);
        CHECK(r3.size() == 1 && std::fabs(r3[0].pos.x - 0.0f) < 1e-3f, "T13_single_snapshot");
    }

    // §15 输入采样
    {
        Input input;
        input.SetKey(32, true);                         // 按下空格
        auto s1 = input.Sample();
        CHECK(s1.held.count(32) == 1, "T15_held");
        CHECK(s1.just_pressed.count(32) == 1, "T15_just_pressed");
        auto s2 = input.Sample();                        // 边沿已清空
        CHECK(s2.just_pressed.count(32) == 0, "T15_edge_cleared");
        input.SetKey(32, false);
        auto s3 = input.Sample();
        CHECK(s3.held.count(32) == 0, "T15_released");
    }

    // §20 配置加载（JSON）+ 缺省回退
    {
        const std::string path = "tmp_client_cfg.json";
        {
            std::ofstream out(path);
            out << R"({
  "width": 1920, "height": 1080, "window_mode": "fullscreen",
  "target_fps": 60, "fixed_fps": 60, "quality": "high", "vsync": false,
  "network": {
    "gateway_addr": "127.0.0.1:9001", "codec": 0,
    "connect_timeout_ms": 2500, "recv_timeout_ms": 800,
    "request_timeout_ms": 4000, "heartbeat_interval_ms": 4000,
    "reconnect_enabled": false, "max_reconnect_attempts": 7,
    "reconnect_base_delay_ms": 250
  }
})";
        }
        ClientConfig cfg;
        CHECK(cfg.Load(path), "T20_load_ok");
        CHECK(cfg.width == 1920 && cfg.height == 1080, "T20_resolution");
        CHECK(cfg.window_mode == WindowMode::Fullscreen, "T20_window_mode");
        CHECK(cfg.quality == QualityTier::High, "T20_quality");
        CHECK(cfg.vsync == false, "T20_vsync");
        CHECK(cfg.net.addr == "127.0.0.1:9001", "T20_net_addr");
        CHECK(cfg.net.connect_timeout_ms == 2500, "T20_net_timeout");
        CHECK(cfg.net.reconnect_enabled == false, "T20_net_reconnect_off");
        CHECK(cfg.net.max_reconnect_attempts == 7, "T20_net_retries");
        std::remove(path.c_str());

        // 缺失文件 -> 缺省值 + 返回 false
        ClientConfig def;
        bool ok = def.Load("nonexistent_config_file.json");
        CHECK(!ok, "T20_missing_returns_false");
        CHECK(def.width == 1280 && def.height == 720, "T20_default_resolution");
    }

    // §18 协议往返（真实 TCP + 真实 Envelope 编解码）
    {
        test::MockServer srv;
        auto pr = srv.Start(0);
        CHECK(pr.HasValue(), "T18_server_start");
        NetConfig nc;
        nc.addr = srv.Addr();
        nc.reconnect_enabled = false;
        nc.request_timeout_ms = 3000;
        nc.recv_timeout_ms = 500;
        NetClient cli(nc);
        CHECK(cli.Connect().HasValue(), "T18_connect");
        auto resp = cli.Request("login");
        CHECK(resp.HasValue(), "T18_request_ok");
        if (resp.HasValue()) {
            protocol::FlatbufCodec codec;
            auto dec = codec.Decode(std::string_view(
                reinterpret_cast<const char*>(resp.Value().data()), resp.Value().size()));
            CHECK(dec.HasValue(), "T18_decode_resp");
            if (dec.HasValue()) {
                CHECK(dec.Value().view().payload == "login", "T18_payload_echo");
                CHECK(dec.Value().view().message_type == protocol::EnvelopeMessageType::Response,
                      "T18_is_response");
            }
        }
        cli.Disconnect();
        srv.Stop();
    }

    // §18+§13 集成：服务器推送快照 Event，客户端解码 + 插值
    {
        test::MockServer srv;
        auto pr = srv.Start(0);
        CHECK(pr.HasValue(), "T18i_server_start");
        WorldSnapshot snap1; snap1.server_time_ms = 0;
        snap1.entities.push_back(EntityPose{1, Vec3{0,0,0}, {}, 0.0f, 0});
        WorldSnapshot snap2; snap2.server_time_ms = 100;
        snap2.entities.push_back(EntityPose{1, Vec3{10,0,0}, {}, 0.0f, 100});
        std::vector<std::uint8_t> e1 = ClientWorld::EncodeSnapshot(snap1);
        std::vector<std::uint8_t> e2 = ClientWorld::EncodeSnapshot(snap2);
        srv.SetAutoEvents({e1, e2});

        NetConfig nc; nc.addr = srv.Addr(); nc.reconnect_enabled = false;
        nc.request_timeout_ms = 3000; nc.recv_timeout_ms = 500;
        NetClient cli(nc);
        CHECK(cli.Connect().HasValue(), "T18i_connect");
        CHECK(cli.Request("login").HasValue(), "T18i_login_triggers_events");

        std::vector<std::uint8_t> b1, b2;
        CHECK(DrainEvent(cli, b1, 2000), "T18i_event_1");
        CHECK(DrainEvent(cli, b2, 2000), "T18i_event_2");
        ClientWorld w;
        w.ApplySnapshot(ClientWorld::DecodeSnapshot(std::string_view(
            reinterpret_cast<const char*>(b1.data()), b1.size())));
        w.ApplySnapshot(ClientWorld::DecodeSnapshot(std::string_view(
            reinterpret_cast<const char*>(b2.data()), b2.size())));
        auto d1 = ClientWorld::DecodeSnapshot(std::string_view(
            reinterpret_cast<const char*>(b1.data()), b1.size()));
        auto r = w.Interpolate(50);
        CHECK(r.size() == 1 && std::fabs(r[0].pos.x - 5.0f) < 0.5f, "T18i_interp_after_snapshot");
        cli.Disconnect();
        srv.Stop();
    }

    // §19 断网重连：DropClient 触发自动重连，后续请求成功；服务端关闭 -> Disconnected
    {
        test::MockServer srv;
        auto pr = srv.Start(0);
        CHECK(pr.HasValue(), "T19_server_start");
        NetConfig nc;
        nc.addr = srv.Addr();
        nc.reconnect_enabled = true;
        nc.max_reconnect_attempts = 3;
        nc.reconnect_base_delay_ms = 50;
        nc.connect_timeout_ms = 500;
        nc.request_timeout_ms = 2000;
        nc.recv_timeout_ms = 200;
        NetClient cli(nc);
        CHECK(cli.Connect().HasValue(), "T19_connect");
        CHECK(RequestRetry(cli, "a"), "T19_first_request");
        srv.DropClient();  // 模拟网络中断
        // 等待客户端检测断线并自动重连
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        CHECK(RequestRetry(cli, "b"), "T19_reconnect_ok");
        CHECK(cli.metrics().reconnects >= 1, "T19_reconnect_count");

        // 服务端彻底关闭：重连耗尽 -> Disconnected
        // 服务端彻底关闭：重连耗尽 -> Disconnected。轮询等待，避免固定 sleep 在
        // 慢环境下（重连多次退避 + connect 超时累加）耗时超过阈值而误判。
        srv.Stop();
        {
            int tries = 0;
            while (cli.state() != NetState::Disconnected && tries < 40) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                ++tries;
            }
        }
        CHECK(cli.state() == NetState::Disconnected, "T19_disconnected_after_stop");
        auto r = cli.Request("c");
        CHECK(!r.HasValue(), "T19_request_after_stop_fails");
        cli.Disconnect();
    }

    tprint::LineFmt("SUMMARY passed=%d failed=%d\n", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}
