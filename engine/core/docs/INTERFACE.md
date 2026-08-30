# engine/core · 公开接口契约（TASK-001 / TASK-002 / TASK-003 / TASK-004）

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

---

# 四、Core Memory / Thread / Scheduler（TASK-004）

## 头文件位置

```
engine/core/include/mmo/core/thread/task.h        # TaskFn（std::function 替代，零堆分配）
engine/core/include/mmo/core/thread/mpmc_queue.h  # MpmcQueue（Vyukov 无锁有界队列）
engine/core/include/mmo/core/thread/thread.h      # Thread（四类角色 + 有界队列 + 优雅停止）
engine/core/include/mmo/core/sched/scheduler.h    # Scheduler（最小堆定时器，宿主线程驱动）
engine/core/include/mmo/core/memory/object_pool.h # ObjectPool（分块定长对象池）
engine/core/include/mmo/core/memory/memory_pool.h # MemoryPool（定长块 + 跨线程有界归还）
engine/core/include/mmo/core/memory/arena.h       # Arena（帧内 bump 分配器）
```

内部实现（**禁止**被下游 `#include`）：`src/thread/thread.cpp`、`src/sched/scheduler.cpp`、
`src/memory/{memory_pool.cpp, arena.cpp}`。

## 公开接口（节选）

```cpp
// ---- 任务 ----
class TaskFn final {                       // 32 字节内联，零堆分配
public:
    static constexpr std::size_t kInlineCapacity = 32;
    TaskFn() noexcept = default;
    template <typename F> TaskFn(F&& f) noexcept;   // 捕获必须 ≤32B、noexcept 构造/移动
    void operator()() noexcept;             // 不清空自身（周期定时器靠重复调用）
    bool Empty() const noexcept;
};

template <typename T>
class MpmcQueue final {                     // Vyukov 无锁有界队列
public:
    explicit MpmcQueue(std::size_t capacity);   // 向上取整到 2 的幂
    bool TryPush(T&&) noexcept;            // 满返回 false（不阻塞、不丢任务）
    bool TryPop(T&) noexcept;              // 空返回 false
    std::size_t Capacity() const noexcept;
    std::size_t Size() const noexcept;     // 瞬时近似，仅指标用
};

// ---- 线程 ----
enum class ThreadRole : std::uint8_t { Network, Simulation, Worker, Persistence };
const char* ToString(ThreadRole) noexcept;

class Thread final {
public:
    struct Config { ThreadRole role; std::string name; std::size_t queue_capacity{4096}; };
    static Result<std::unique_ptr<Thread>> Create(Config, std::function<void()> on_start = {});
    Result<void> Post(TaskFn task);          // 队列满 → ErrorCode::BUSY（不阻塞不丢）
    Result<void> PostBlocking(TaskFn task);  // 阻塞重试，禁止在 Tick 内调用
    void RequestStop() noexcept;             // 幂等；已提交任务仍跑完
    void Join();
    bool JoinFor(std::chrono::milliseconds);
    ThreadRole Role() const noexcept;
    std::size_t Pending() const noexcept;     // 队列深度（指标）
    std::size_t Executed() const noexcept;    // 已执行数（指标）
    bool StopRequested() const noexcept;
};

// ---- 定时器 ----
class Scheduler final {
public:
    static constexpr TimerId kInvalidTimerId = 0;
    static constexpr std::uint32_t kMaxCatchUpPerTick = 8;   // 单 Tick 周期补触发上限
    Result<TimerId> ScheduleAfter(DurationMs delay, TaskFn);
    Result<TimerId> ScheduleAt(SteadyTime when, TaskFn);
    Result<TimerId> ScheduleEvery(DurationMs period, TaskFn);
    Result<TimerId> ScheduleEveryAt(SteadyTime first, DurationMs period, TaskFn);
    Result<void> Cancel(TimerId);            // 幂等
    Result<std::size_t> Tick(SteadyTime now); // 宿主驱动；now 倒流 → INVALID_ARGUMENT
    std::size_t ReadyCount() const noexcept;  // O(n) 滞后指标，禁放每帧热路径
    std::size_t TimerCount() const noexcept;
    std::size_t LastFired() const noexcept;
    std::size_t FailedFires() const noexcept; // 回调异常累计
    std::size_t SlotCount() const noexcept;
};

// ---- 内存 ----
template <typename T, std::size_t Chunk = 4096>
class ObjectPool final {                     // 单线程拥有；无锁无原子
public:
    explicit ObjectPool(std::size_t prewarm = 0);
    T* Acquire();                            // 默认构造；池空自动扩容
    template <typename... Args> T* Acquire(Args&&...);  // 带参构造
    void Release(T* ptr) noexcept;           // 析构但内存留池
    std::size_t Capacity()/InUse()/FreeCount()/ChunkCount() const noexcept;
};

class MemoryPool final {                     // 拥有者线程无锁；跨线程有界归还
public:
    explicit MemoryPool(std::size_t block_size, std::size_t blocks_per_chunk = 1024);
    void* Allocate(std::size_t bytes);
    void Deallocate(void* ptr, std::size_t bytes) noexcept;  // 野指针/双释放只记指标
    std::size_t UsedBytes()/BlockSize()/FreeDepth()/RemoteReturns()/InvalidFrees()/OverflowCount() const noexcept;
};

class Arena final {                          // 单线程拥有；无锁无原子
public:
    explicit Arena(std::size_t bytes);
    void* Push(std::size_t bytes, std::size_t align = 8);  // 不足返回 nullptr
    void Reset() noexcept;                    // 整块回收，内存保留复用
    std::size_t UsedBytes()/CapacityBytes()/BlockCount() const noexcept;
};
```

## 线程归属红线（TASK-004 核心约束）

- **Scheduler 不创建任何线程**：`engine/core/{src/sched,include/mmo/core/sched}` 下
  任何文件（含注释）出现 `std::thread` 字面量即验收失败。定时器由宿主线程
  （SimulationThread / WorkerThread）在自己的循环里调用 `Tick(now)` 驱动。
- **Scheduler 非线程安全**：所有方法必须在同一个线程调用（§4 State Owner）。
- **Thread 四类角色固定**：禁止私自新增第五类（`ThreadRole` 序列化稳定）。
- **ObjectPool / Arena 单线程拥有**：禁止跨线程共享（热路径无锁，跨线程 = 数据竞争）。
  跨线程复用内存用 `MemoryPool`（它自带跨线程归还队列）。

## 语义约束

- **周期定时器 `period <= 0` 直接拦下**（入口返回 `INVALID_ARGUMENT`），
  否则 `Tick` 的 catch-up 会无限触发。
- **`Tick(now)` 的 `now` 必须单调不减**：倒流 = 多半误用墙钟，当场 `INVALID_ARGUMENT`。
- **周期定时器一次 Tick 最多补 `kMaxCatchUpPerTick=8` 次**；命中限幅后 deadline 快进到
  `now` 之后丢弃积压，避免死亡螺旋。`TestSchedulerPeriodic` 验证 `Tick(+120ms)` 严格返回 8。
- **`Cancel` 幂等**：取消不存在 / 已触发 / 已取消的 id 一律 `Ok`。
- **惰性删除**：Cancel 只打 `cancelled` 标记，槽位回收在 `Tick` 弹出或 `Compact()` 时发生；
  `cancelled_count_ == heap_.size()`（全取消）时整体回收，避免 `slots_` 无限增长。
- **`TaskFn` 只可移动不可拷贝**：任务所有权唯一，避免意外多次执行。
- **`MemoryPool::Deallocate` 对野指针 / 双释放只记 `InvalidFrees` 指标，不崩溃**
  （魔数校验清零，重复释放被识别成野指针）。

## 禁止

- **禁止 Scheduler 创建 / 使用执行线程**（红线静态扫描强制）。
- **禁止 `ObjectPool` / `Arena` 跨线程共享**。
- **禁止 `Thread` 新增第五类角色**。
- **禁止周期定时器 `period <= 0`**。
- **禁止 `Tick` 的 `now` 倒流**（会触发 `INVALID_ARGUMENT`）。
- **禁止 `__int128`**（`-Wpedantic`）；禁止 `engine/` 内 `std::cout` / `printf` /
  `std::thread`（sched 目录）。
- **禁止把 `src/thread/thread.cpp`、`src/sched/scheduler.cpp`、`src/memory/*.cpp`
  暴露给下游**：它们只含实现，调用方只能依赖上面列出的公开头。
