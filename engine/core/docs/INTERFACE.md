# engine/core · 公开接口契约（TASK-001 / TASK-002 / TASK-003）

> 本文件为 `STATUS: DONE` 后冻结的对外契约。下游依赖此接口；破坏性变更须走
> `version` 字段 + 兼容性评估，禁止静默改签名导致下游编译失败。
>
> - **TASK-001** · Core Error / Result 系统 —— `mmo/core/error/*`
> - **TASK-002** · Core Logger / Trace —— `mmo/core/log/*`
> - **TASK-003** · Core Time / UUID / Config —— `mmo/core/{time,uuid,config}/*`

---

# 一、Core Error / Result（TASK-001）

## 头文件位置

```
engine/core/include/mmo/core/error/error_code.h
engine/core/include/mmo/core/error/error.h
engine/core/include/mmo/core/error/result.h
```

下游只包含以上公开头；禁止 `#include` `engine/core/src/` 下的任何文件。

## 公开接口

```cpp
namespace mmo::core {

enum class ErrorCode : int16_t {
  OK = 0, INVALID_ARGUMENT = 1, NOT_FOUND = 2, TIMEOUT = 3, BUSY = 4,
  VERSION_CONFLICT = 5, UNAUTHORIZED = 6, RATE_LIMITED = 7, INTERNAL_ERROR = 8,
};

// 数值 -> 名称（未知值返回 "UNKNOWN"）
const char* ToString(ErrorCode code) noexcept;

// 名称 -> 数值；未知名称或越界数值返回 nullopt（不崩溃）
std::optional<ErrorCode> FromString(std::string_view name) noexcept;

// 可重试判定：TIMEOUT / BUSY / RATE_LIMITED = true
bool IsRetryable(ErrorCode code) noexcept;

// 受控错误域常量集合
namespace domain {
  inline constexpr std::string_view kCore, kNet, kScene, kCombat,
                                    kData, kEconomy, kLua;
}
bool IsValidDomain(std::string_view dom) noexcept;

class Error final {                       // 值语义，无异常
 public:
  Error(ErrorCode c, std::string_view msg, std::string_view domain = domain::kCore);
  ErrorCode Code() const noexcept;
  std::string_view Message() const noexcept;   // <=32 字节内联，不分配堆
  std::string_view Domain() const noexcept;
  std::string ToString() const;                // "domain/CODE: message"
  bool IsRetryable() const noexcept;
};

template <typename T> class [[nodiscard]] Result {
 public:
  static Result Ok(T v);
  static Result Fail(Error e);
  bool HasValue() const noexcept;
  explicit operator bool() const noexcept;
  const T& Value() const&;          // 无值时 assert + 终止（非抛异常）
  T&& Value() &&;
  const Error& Err() const&;
  Error&& Err() &&;
  T ValueOr(T fallback) const;
  template <typename F> auto AndThen(F&& f);   // 链式；失败短路透传
  template <typename F> auto Map(F&& f);
};

template <> class [[nodiscard]] Result<void>;  // 特化：只关心成功/失败

}  // namespace mmo::core

// MMO_TRY：等价于 Rust 的 ?。expr 返回 Result<T>；
// 成功展开为 T 值，失败提前 return 同类型 Result<T>::Fail(err)。
#define MMO_TRY(expr) ({ auto _mmo_r=(expr); if(!_mmo_r) \
  return decltype(_mmo_r)::Fail(std::move(_mmo_r).Err()); std::move(_mmo_r).Value(); })
```

## 语义约束

- `Result` 内部为 `std::variant<T, Error>`，互斥持有；`Ok` 只存 `T`，`Fail` 只存 `Error`。
- 失败路径（`Fail` 构造、复制、`AndThen`/`Map` 短路）在短 `message` 下零堆分配。
- `Error::Message()` 返回 `std::string_view`，指向内联缓冲或堆副本；`message` 超长（>32 字节）
  回退到堆，不会截断溢出。
- `Value()` 在 `!HasValue()` 时 `assert` 失败（Debug）并终止，不使用异常。
- `MMO_TRY` 需要 GNU 语句表达式扩展（`engine/core` 子树已开启 `CXX_EXTENSIONS`）。

## 禁止

- 禁止用 C++ 异常作为业务错误传播机制（边界转换除外）。
- 禁止任何模块自行定义新的顶层 `ErrorCode` 枚举。
- 禁止在 `Error` 构造中做 IO、加锁或分配大对象。
- 禁止 `Result` 失败路径产生堆分配。
- 禁止用 `int` / `bool` 返回值代替 `Result`。

---

# 二、Core Logger / Trace（TASK-002）

## 头文件位置

```
engine/core/include/mmo/core/log/log_level.h
engine/core/include/mmo/core/log/trace_id.h
engine/core/include/mmo/core/log/log_record.h
engine/core/include/mmo/core/log/log_context.h
engine/core/include/mmo/core/log/log_sink.h
engine/core/include/mmo/core/log/logger.h
```

下游只包含以上公开头。以下**不是**公开接口，禁止下游 `#include`：
`engine/core/src/log/async_ring_buffer.h`、`engine/core/src/log/log_formatter.h`。

## 公开接口

```cpp
namespace mmo::core {

// ---- 级别 ----
enum class LogLevel : uint8_t { Trace=0, Debug=1, Info=2, Warn=3, Error=4, Fatal=5 };
const char*            ToString(LogLevel) noexcept;            // 5 字符定宽显示名
std::optional<LogLevel> ParseLogLevel(std::string_view) noexcept;  // 配置串 -> 级别

// ---- TraceID（uint64，可排序，非随机）----
//   布局：(node_id << 48) | (timestamp_low << 16) | counter
using TraceID   = std::uint64_t;
using RequestID = std::uint64_t;
constexpr TraceID kInvalidTraceId = 0;

void     SetNodeId(std::uint16_t node) noexcept;
std::uint16_t NodeId() noexcept;
TraceID  NewTraceID() noexcept;                 // 严格单调递增
RequestID DeriveRequestID(TraceID t) noexcept;  // 低 48 位继承，高位自增
std::uint16_t NodeOf(std::uint64_t id) noexcept;
std::uint64_t LowOf(std::uint64_t id) noexcept;

// ---- 线程上下文 ----
struct LogContext {
  TraceID         trace_id{kInvalidTraceId};
  RequestID       request_id{kInvalidRequestId};
  PlayerID        player_id{kInvalidPlayerId};
  SceneID         scene_id{kInvalidSceneId};
  std::string_view module{};   // 必须是静态存储期字面量，禁止运行时拼接
};
const LogContext& CurrentLogContext() noexcept;
class ScopedLogContext;                        // RAII：进入覆盖、退出恢复
template <typename Fn> decltype(auto) WithContext(const LogContext&, Fn&&);

// ---- Sink ----
class ILogSink {
 public:
  virtual void Write(const LogRecord&) noexcept = 0;
  virtual void Flush() noexcept {}
};

// ---- 配置与门面 ----
struct LoggerConfig {
  std::string service{"gamenode"};
  LogLevel    level{LogLevel::Info};
  bool        console{true};
  bool        json{false};
  std::string file_path{};              // 空 = 不写文件
  std::size_t file_max_size{64u<<20};   // 单文件滚动阈值
  std::uint32_t file_max_files{8};      // 保留份数
  std::size_t queue_capacity{32768};    // 环形队列槽位数
};

class Logger {                                   // 静态门面，禁止实例化
 public:
  static Result<void> Init(const LoggerConfig&); // 重复 Init -> BUSY
  static void Shutdown() noexcept;
  static bool IsInitialized() noexcept;
  static void RegisterSink(std::shared_ptr<ILogSink>);
  static void SetLevel(LogLevel) noexcept;
  static bool ShouldLog(LogLevel) noexcept;      // 热路径短路判断
  static void Flush() noexcept;                  // 等待后台线程排空
  static void Write(LogLevel, const LogRecord&) noexcept;
  static std::uint64_t EnqueuedCount() noexcept;
  static std::uint64_t DroppedCount() noexcept;
  static std::uint64_t WrittenCount() noexcept;
};

}  // namespace mmo::core

// 调用点只写格式串；宏内部先 ShouldLog，再格式化。
#define MMO_LOG(level, fmt, ...) ...
#define MMO_LOG_TRACE(fmt, ...)  MMO_LOG(::mmo::core::LogLevel::Trace, fmt, ##__VA_ARGS__)
#define MMO_LOG_DEBUG(fmt, ...)  ...
#define MMO_LOG_INFO(fmt, ...)   ...
#define MMO_LOG_WARN(fmt, ...)   ...
#define MMO_LOG_ERROR(fmt, ...)  ...
#define MMO_LOG_FATAL(fmt, ...)  ...   // 立即 Flush 落盘
```

## 语义约束

- **九项固定字段**：`timestamp_ns` / `level` / `service` / `module` / `trace_id` /
  `request_id` / `player_id` / `scene_id` / `message`，外加 `thread_id`。
  文本模式以 `key=` 前缀输出，JSON 模式 key 与之一一对应。
- **日志关闭时零成本**：`MMO_LOG` 展开后先判 `ShouldLog`，不通过则整条语句不执行，
  实测 0.509 ns/次（Release），业务代码可无条件埋点。
- **业务线程永不碰磁盘**：只做「格式化 + 无锁入队」，文件 IO 全在后台线程。
- **队列满即丢弃**：`dropped` 计数单调增加，按「一次突发一条 Warn」收敛告警，
  业务线程绝不阻塞。
- **`LogContext::module` 必须是静态存储期字面量**（`static constexpr` / 字符串字面量），
  运行时拼接会破坏「单条日志零堆分配」。
- **`Fatal` 级别立即 `Flush()`**，进程可能马上崩溃，不等后台线程。

## 禁止

- 禁止在业务线程做文件 IO 或加锁写盘。
- 禁止 `std::cout` / `printf` / `std::cerr` 直接输出（全仓红线，含 `engine/` 下的测试代码）。
- 禁止用随机数生成 `TraceID`（必须 `node_id + 单调时钟 + 计数`，可排序）。
- 禁止先拼字符串再判断日志级别（必须先 `ShouldLog`）。
- 禁止日志内容包含明文口令、令牌、完整身份证 / 银行卡等敏感数据。
- 禁止日志队列满时阻塞业务线程。

# 三、Core Time / UUID / Config（TASK-003）

## 头文件位置

```
engine/core/include/mmo/core/time/clock.h         # MonotonicClock / WallClock
engine/core/include/mmo/core/time/tick_clock.h    # TickClock（纯整数递推）
engine/core/include/mmo/core/time/timer.h         # ITimerQueue 抽象接口（TASK-004 实现）
engine/core/include/mmo/core/uuid/uuid.h          # Uuid（V4 / V7）
engine/core/include/mmo/core/config/config_manager.h  # ConfigManager（静态 API）
```

内部实现（**禁止**被下游 `#include`）：

```
engine/core/src/time/wall_clock_seam.h   # 测试注入缝：SetInjectedWallClockNanos
engine/core/src/uuid/entropy.h           # OS 熵源封装
engine/core/src/config/json_parser.h     # 极简递归下降 JSON 解析器
engine/core/src/config/config_snapshot.h # 不可变配置快照
```

## 公开接口

```cpp
// ---- 时钟 ----
using SteadyNs   = std::int64_t;                       // 单调纳秒
using SteadyTime = std::chrono::steady_clock::time_point;
using DurationMs = std::chrono::duration<std::int64_t, std::milli>;

class MonotonicClock final {
public:
    static SteadyNs   Now()     noexcept;              // QPC，实测 16.8 ns/次
    static SteadyTime Point()   noexcept;              // 实测 16.8 ns/次
    static SteadyNs   Elapsed(SteadyTime from) noexcept;
};

class WallClock final {                                // 墙钟：只用于落盘 / 展示 / 跨机对齐
public:
    static std::int64_t UnixNanos() noexcept;          // 可被测试注入覆盖
    static std::int64_t UnixMillis() noexcept;
};

// ---- Tick ----
class TickClock final {
public:
    static constexpr std::uint32_t kMaxCatchUpSteps = 3;   // 单帧最多补 3 个 Tick
    static constexpr std::uint32_t kDefaultHz       = 20;  // 50ms / Tick

    explicit TickClock(std::uint32_t hz) noexcept;         // hz == 0 兜底为 1
    std::uint32_t Hz() const noexcept;
    SteadyNs      TickIntervalNs() const noexcept;
    DurationMs    TickInterval() const noexcept;

    SteadyTime    NextTickDeadline(SteadyTime prev) const noexcept;  // 纯递推，不读时钟
    std::uint32_t CatchUpSteps(SteadyTime now, SteadyTime prev) const noexcept;
};

// ---- UUID ----
class Uuid final {
public:
    std::array<std::uint8_t, 16> bytes{};

    static Uuid Nil() noexcept;
    static Result<Uuid> TryNewV4() noexcept;   // OS CSPRNG
    static Uuid         NewV4() noexcept;      // 失败返回 Nil，绝不降级为弱随机
    static Result<Uuid> TryNewV7() noexcept;   // 48bit ms 前缀 + CSPRNG
    static Uuid         NewV7() noexcept;

    std::string          ToString() const;                 // 36 字符带连字符
    static Result<Uuid>  Parse(std::string_view text);     // 格式/长度非法 → INVALID_ARGUMENT
    int      Version() const noexcept;   // 4 或 7
    int      Variant() const noexcept;   // RFC 9562 variant（2）
    bool     IsNil() const noexcept;
    std::int64_t TimestampMillis() const noexcept;  // 仅 V7 有意义
};

// ---- Config ----
class ConfigManager final {
public:
    static Result<void> LoadFile(std::string_view path);
    static Result<void> LoadDir(std::string_view dir);   // 按字典序合并 *.json
    static Result<void> Reload();                         // 失败保留旧快照

    template <typename T>
    static Result<T> Get(std::string_view key) noexcept;  // 读路径无锁

    static Result<void> Set(std::string_view key, std::string_view value);
    static std::uint64_t Version() noexcept;              // 单调递增
    static bool   Contains(std::string_view key) noexcept;
    static std::vector<std::string> Keys();
    static std::size_t Size() noexcept;
    static void ResetForTest();
};
```

## 语义约束

- **`TickClock` 是纯整数递推**：`NextTickDeadline(prev) = prev + interval_ns`。
  它**不读任何时钟**，因此墙钟回拨、NTP 跳变、闰秒、虚拟机挂起恢复都对它无效。
  实测跑 10000 个 Tick，累计误差 **0 ns**（理论 500 s vs 实测 500 s）。
- **`MonotonicClock` 用 QPC + 定点乘移**，不用 `std::chrono::steady_clock`。
  实测：`steady_clock::now()` 24.5 ns/次（阈值 25 ns，只剩 2% 余量）、
  裸 QPC 15.7 ns、QPC + 定点 16.9 ns。选 QPC 定点是为了把余量从 2% 拉到 33%。
  定点换算用 4 项 64 位部分积实现，**不用 `__int128`**（会触发 `-Wpedantic`）。
- **配置读路径零原子 RMW**：热路径用 thread-local 缓存持有快照 `shared_ptr` 强引用
  + 一个 `uint64_t` 版本号比对；直接 `atomic<shared_ptr>::load` 每次要 2 次原子 RMW，
  实测 34 ns 的预算扛不住。写路径（Load/Reload/Set）才走互斥锁，是冷路径。
- **快照整体替换**：`Reload()` 先完整构建新快照，成功才原子换指针。
  任何一步失败（文件损坏 / 目录消失 / 重复 key）都返回 Error，**旧快照原样保留**，
  不存在半替换状态。4 读线程并发压测 105 万次读，异常值 **0**。
- **重复 key 是错误**：同一批加载里同一 key 出现两次 → `INVALID_ARGUMENT`
  （绝不允许「后写的悄悄覆盖先写的」）。
- **UUID 熵源失败不降级**：`FillRandom` 失败时 `TryNew*` 返回 `INTERNAL_ERROR`，
  `New*` 返回 `Uuid::Nil()`，**绝不用 `rand()` / 时间戳凑数**。
- **V7 只申请 10 字节熵**：前 6 字节是毫秒时间戳，会被覆写，
  只给 `bytes[6..16)` 取随机，把 `uuid_v7_ns` 从 72.8 ns 降到 67.0 ns。
- **`ITimerQueue` 本任务只定义接口不实现**，TASK-004 Scheduler 负责实现。
  接口纯抽象，测试里用 `NullTimerQueue` 证明其可实现。

## 禁止

- **禁止用墙钟驱动 Tick**：Tick 相关路径不得出现 `WallClock` 或
  `std::chrono::system_clock`（`engine/core/src/time` 与
  `engine/core/include/mmo/core/time` 已被验收脚本红线扫描覆盖）。
- **禁止在 `NextTickDeadline` 里读时钟**：必须是纯递推，否则漂移会累积。
- **禁止 `CatchUpSteps` 超过 `kMaxCatchUpSteps`**：帧率一旦跟不上必须限幅，
  否则「补 Tick → 更慢 → 补更多 Tick」形成死亡螺旋。
- **禁止 `__int128`**（`-Wpedantic`）；禁止在 `engine/` 内出现 `std::cout` / `printf`。
- **禁止把 `src/config/config_snapshot.h`、`src/config/json_parser.h`、
  `src/time/wall_clock_seam.h`、`src/uuid/entropy.h` 暴露给下游**：
  `ConfigManager` 头里只前向声明 `detail::ConfigSnapshot`，调用方无法改动它。
- **禁止配置热更返回半替换快照**；**禁止重复 key 静默生效**。
