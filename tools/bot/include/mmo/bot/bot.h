#pragma once

/// TASK-038 §7 · Bot Framework 公共接口（协议层，无渲染客户端）
///
/// 设计要点：
///   - Bot 为协议层客户端，复用 TASK-005（mmo::protocol）做 Envelope 编解码；
///     连接用自带的 winsock 客户端，采用与 mmo::net 对称的「4 字节大端长度前缀 + payload」帧格式。
///   - Bot 与 BotFarm 自身状态由各自实例拥有（无共享写者），符合 PROJECT_REQUIREMENTS §10/§12。
///   - sim 模式：BotFarm 内嵌 MockGateway（进程内 winsock 服务端，回合法 Envelope
///     Response），使 bot_bench 可在无真实服务端的情况下跑通真实 TCP + 真实编解码，
///     度量 Bot 动作处理 tick 与错误率。真实集成阶梯（tools/load）通过 --gateway 指定真实网关节点。
///
/// 头文件只暴露公共 API；具体实现见 src/。禁止下游 #include src/。

#include "mmo/core/error/result.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace mmo { namespace bot {

/// 动作周期类型（与任务书 §7 一致）。
enum class BotAction : std::uint8_t {
    Login = 0,
    Move,
    Attack,
    Quest,
    Trade,
    Chat,
    Logout,
    Reconnect,
};

/// 时长类型（与 core::DurationMs 等价，便于调用方直接传 std::chrono::milliseconds）。
using DurationMs = std::chrono::milliseconds;

/// 单个 Bot 的脚本：动作序列 + 每个动作后的思考延迟 + 循环次数。
struct BotScript {
    std::vector<BotAction> actions;
    std::vector<DurationMs> delays;  // 与 actions 等长；动作之间的思考延迟，不计入 tick。
    std::uint32_t loop{1};
};

/// 单个 Bot 的运行统计。
struct BotStats {
    std::uint64_t actions_done{0};
    std::uint64_t errors{0};
    double rtt_ms_p95{0};       // 请求-响应往返 P95（毫秒）
    double received_pps{0};     // 每秒收到的响应数
};

/// BotFarm 聚合统计（bot_bench 直接产出）。
struct AggregateStats {
    std::uint32_t bots{0};
    std::uint64_t actions_done{0};
    std::uint64_t errors{0};
    double tick_p50_ms{0};   // 协议处理热路径成本（编码+解码+校验）的 P50，不含网络等待
    double tick_p95_ms{0};
    double tick_p99_ms{0};
    double tick_max_ms{0};
    double rtt_ms_p95{0};    // 完整动作周期（含 send/recv 网络往返）的 P95，供参考
    double received_pps{0};
    double error_rate{0};    // errors / actions_done
};

/// Bot 运行配置。
struct BotConfig {
    std::string gateway_addr{"127.0.0.1:9001"};
    std::uint32_t codec{0};        // 0 = FlatBuffers（默认，零拷贝），1 = Protobuf
    bool sim_mode{true};           // true：BotFarm 内嵌 MockGateway
    bool include_reconnect{true};  // 默认脚本是否含 Reconnect（每轮末重连）；CCU 时延压测建议关
    DurationMs connect_timeout{2000};
    std::uint32_t worker_seed{1};  // 多 Bot 错峰用
    DurationMs think_scale{1};     // delays 的缩放系数（0 = 不思考，纯吞吐压测）
};

/// 协议层 Bot：独立的单连接客户端，跑完一段脚本后汇报统计。
class Bot {
public:
    /// 连接 gateway_addr，按 script 执行 loop 遍，直到完成或 stop 置位。
    core::Result<void> Run(const BotScript& script, std::string_view gateway_addr);
    BotStats Stats() const noexcept { return stats_; }

    /// 内部驱动：带停止标志，供 BotFarm 多线程度用。bot 自身不持有停止标志。
    /// codec: 0=FlatBuffers 1=Protobuf；tick_sink 非空时把每次动作「协议处理热路径成本(ms)」
    /// （编码+解码+校验，不含网络等待）写入，供农场聚合。
    core::Result<void> RunWith(std::string_view gateway_addr,
                               const BotScript& script,
                               const std::atomic<bool>* stop,
                               std::uint32_t codec = 0,
                               std::vector<double>* tick_sink = nullptr);

private:
    BotStats stats_;
};

/// 单机 Bot 农场：批量拉起 N 个 Bot（每 Bot 单线程事件驱动），统一驱动与聚合。
class BotFarm {
public:
    BotFarm();
    /// 拉起 count 个 Bot 的准备（含 sim 模式下启动 MockGateway）。
    core::Result<void> Spawn(std::uint32_t count, const BotConfig& cfg);
    /// 全部 Bot 并发运行 dur 毫秒后停止，返回聚合统计。
    core::Result<AggregateStats> RunUntil(DurationMs dur);
    /// 优雅停止（最多等待 grace）；未完成的 Bot 强制结束。
    core::Result<void> StopAll(DurationMs grace);

    /// 取内嵌 MockGateway 实际监听地址（sim 模式有效）。
    std::string SimGatewayAddr() const noexcept { return sim_addr_; }

    ~BotFarm();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    std::string sim_addr_;
};

/// 工具：把动作枚举转字符串（用于 Envelope payload）。
std::string_view ActionName(BotAction a) noexcept;

}}  // namespace mmo::bot
