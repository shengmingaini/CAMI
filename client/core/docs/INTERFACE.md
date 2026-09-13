# client/core 公共接口契约（TASK-034）

> 本文件描述 `client/core/include/mmo/client/` 下冻结的对外接口。下游（runtime / gameplay / ui /
> extensions）只能通过这些公开头调用，禁止 `#include` `src/` 或内部头。

## 1. GameLoop（固定步长循环）

```cpp
namespace mmo::client;

struct GameLoopConfig {
    core::SteadyNs fixed_dt_ns = 16'666'667;  // 60Hz 默认
    int            max_catchup = 5;            // 单帧最多补算逻辑步（防螺旋死亡）
};

struct FrameStats {
    uint64_t frames = 0, ticks = 0, overshoot = 0, catchups = 0;
    double frame_ms_p50/p95/p99/max = 0;       // 渲染帧间隔百分位
    double tick_ms_p50/p95/p99/max  = 0;       // 逻辑步节拍（相邻 tick 真实毫秒差）
};

class GameLoop {
public:
    using TickFn   = std::function<void(core::SteadyNs now, core::SteadyNs dt)>;
    using RenderFn = std::function<void(core::SteadyNs now, double alpha)>;
    static FrameStats Run(const GameLoopConfig& cfg,
                          uint64_t duration_ms,
                          TickFn tick,
                          RenderFn render = {},
                          const std::atomic<bool>* stop = nullptr);
};
```

- 全程使用 `core::MonotonicClock`，绝不用墙钟驱动。
- `tick_ms_p95` 即 §12「帧精度」验收口径（60Hz 下 ≈ 16.67ms）。

## 2. NetClient（连接 / 重连 / 心跳）

```cpp
struct NetConfig {
    std::string addr;                 // "host:port"
    int  codec = 0;                  // 0=Flatbuf, 1=Protobuf（须与 TASK-005 协商）
    bool reconnect_enabled = true;
    int  max_reconnect_attempts = 5;
    uint32_t reconnect_base_delay_ms = 500;
    uint32_t connect_timeout_ms = 2000;
    uint32_t request_timeout_ms  = 5000;
    uint32_t recv_timeout_ms     = 1000;
    uint32_t heartbeat_interval_ms = 5000;
};

enum class NetState { Disconnected, Connecting, Connected, Reconnecting };

class NetClient {
public:
    explicit NetClient(NetConfig cfg);
    ~NetClient();                                  // 自动 Disconnect
    core::Result<void> Connect();
    void Disconnect() noexcept;
    core::Result<std::vector<uint8_t>> Request(std::string_view payload,
                                               uint32_t timeout_ms = 0);
    core::Result<void> Send(std::string_view payload, EnvelopeMessageType t);
    core::Result<void> SendHeartbeat();
    bool TryRecvEvent(std::vector<uint8_t>& out);  // 主线程轮询 Event 队列
    NetState state() const noexcept;
    const NetMetrics& metrics() const noexcept;
};
```

- 单读线程模型：recv 线程只做搬运 + 解码 + 校验，不解析业务。
- `Request` 阻塞等待匹配 `request_id` 的 Response（超时由 `request_timeout_ms` 控制）。
- 失败且 `reconnect_enabled` 时自动 `Reconnect()`（退避 `base_delay × attempt`），耗尽转入 `Disconnected`。
- 心跳由调用方按 `heartbeat_interval_ms` 驱动（内部不起定时器线程，便于确定性单测）。

## 3. ClientWorld（世界镜像 + 插值）

```cpp
struct Vec3 { float x, y, z; };
struct EntityPose { EntityId id; Vec3 pos, vel; float heading; int64_t ts_ms; };
struct WorldSnapshot { int64_t server_time_ms; std::vector<EntityPose> entities; };

class ClientWorld {
public:
    void ApplySnapshot(const WorldSnapshot& snap);
    std::vector<EntityPose> Interpolate(int64_t render_time_ms);
    static std::vector<uint8_t>  EncodeSnapshot(const WorldSnapshot& snap);
    static WorldSnapshot         DecodeSnapshot(std::string_view bytes);
};
```

- 每个实体保留最近两段快照（`prev` / `curr`），在 `[server_time - interp_buffer_ms]` 处线性插值。
- 外推上限 200ms：最新快照比 `render_time` 旧超过 200ms（网络停滞）→ **冻结**在最新姿态，错误计数 +1。
- `WorldSnapshot` 为客户端自包含二进制（id/pos/vel/heading/ts），与协议层 fbs 解耦。

## 4. Input（输入采样）

```cpp
struct InputSample {
    std::set<int> held;        // 持续按住
    std::set<int> just_pressed;// 本帧刚按下（边沿）
};
class Input {
public:
    void SetKey(int code, bool down);
    InputSample Sample();      // 逻辑帧边界采样，采样后清空 just_pressed 边沿
};
```

- 表现层实时写 `SetKey`，逻辑层每帧 `Sample()` 一次；无窗口依赖，便于单测。

## 5. Config（配置加载）

```cpp
enum class WindowMode { Windowed, Fullscreen, Borderless };
enum class QualityTier { Low, Medium, High, Ultra };

struct ClientConfig {
    uint32_t width = 1280, height = 720;
    WindowMode window_mode = WindowMode::Windowed;
    uint32_t target_fps = 60, fixed_fps = 60;
    QualityTier quality = QualityTier::Medium;
    bool vsync = true;
    struct { /* 网络参数，见 NetConfig 对应字段 */ } net;
    bool Load(const std::string& path);   // 成功 true；缺失/损坏返回 false 并回退安全缺省
};
```

- 解析用自带极简 JSON（无第三方依赖）；字段缺失回退默认值，绝不崩溃。
- 默认配置：`config/client/client.json`（仓库根 `config/client/` 与 `client/config/client/` 各持一份）。

## 6. 协议契约（不可变）

- TASK-005 的 FlatBuffers schema 是客户端唯一契约；`EnvelopeView` / `ICodec` / `EnvelopeValidator`
  来自 `mmo::protocol`，本模块不复写。
- `EncodeSnapshot` / `DecodeSnapshot` 使用 7×`float`（pos.xyz + vel.xyz + heading）定长编码 + 实体数前缀。
