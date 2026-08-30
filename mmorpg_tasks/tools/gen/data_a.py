# -*- coding: utf-8 -*-
"“”TASK-000 ~ TASK-009：Phase 0 工程基础 + Phase 1 统一通信（上）“”"

OWNER = "Codex / WorkBuddy Agent 实施；@技术总监 二次验收；本地 MinGW MSYS2 g++ + vcpkg 编译验证"

TASKS = [
# ------------------------------------------------------------------ 000
dict(
 id="TASK-000", name="项目初始化与仓库规范", phase="Phase 0 · 工程基础",
 objective="建立整个项目的统一工程规则与空工程骨架：目录结构、根 CMake 工程、Debug/Release 双构建、基础 CI、以及 PROJECT_REQUIREMENTS.md / ARCHITECTURE.md / DEVELOPMENT.md 三份最高级规范文档。本任务**不实现任何游戏功能**。",
 deps="无",
 module="build / repo",
 owner=OWNER,
 inp="本实施清单中的《Project Requirements V1.0 — Frozen Architecture》全文；本地 MinGW MSYS2 g++ 工具链；vcpkg baseline aae277ac",
 out="可编译的空工程；冻结的架构与开发规范文档；CI 可运行的构建流水线",
 iface="无业务接口。仅对外暴露构建入口：`cmake -S . -B build/<Type> -DCMAKE_TOOLCHAIN_FILE=$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake`。",
 data="无业务数据结构。仓库元数据：vcpkg.json（manifest mode，baseline aae277ac）",
 thread="无",
 hot="NO", io="NO", rpc="NO", persist="NO",
 files=["CMakeLists.txt", "vcpkg.json", ".gitignore", ".editorconfig", "cmake/*.cmake",
        "engine/", "server/", "game/", "client/", "protocol/", "database/", "scripting/",
        "tests/", "tools/", "docs/", "task/", "scripts/", ".github/workflows/"],
 steps=[
  "初始化 Git 仓库：`git init -b main`，写入 .gitattributes（*.sh text eol=lf，*.md text eol=lf，*.cpp text）",
  "建立根目录结构 engine / server / game / client / protocol / database / scripting / tests / tools / docs / task / scripts，每个目录放 .gitkeep 与 README.md 占位",
  "写 README.md：项目定位（类大型 MMORPG 服务器框架，50k CCU 扩展路线）、进程拓扑图、构建前置条件、快速开始三条命令",
  "写 LICENSE（MIT 或 Apache-2.0，二选一后写死，禁止留 TODO）",
  "写 PROJECT_REQUIREMENTS.md：**原样固化**《Project Requirements V1.0》，并在文件头标注「本文件冻结，修改需 RFC + 人工批准」",
  "写 ARCHITECTURE.md：四进程（Gateway/GameNode/DataService/ControlService）、GameNode 内部模块树、Command/Query/Event 三通信模型、状态 Ownership 表、Tick 阶段划分",
  "写 DEVELOPMENT.md：目录模板（ModuleName/{include,src,tests,benchmark,docs,CMakeLists.txt}）、模块必备五文档（README/INTERFACE/DEPENDENCY/PERFORMANCE/TEST）、错误码表、日志字段规范、依赖单向规则（Game→Gameplay→Core）",
  "创建根 CMakeLists.txt：C++20、`add_subdirectory` 各顶层目录、选项 `MMORPG_BUILD_TESTS=ON`、`MMORPG_BUILD_BENCHMARKS=ON`、`MMORPG_ENABLE_LUA`、`MMORPG_BUILD_CLIENT`",
  "创建 cmake/ 模块：CompilerWarnings.cmake（-Wall -Wextra -Wpedantic，MSVC 用 /W4）、Sanitizers.cmake（Debug 下可选 ASan）、vcpkg triplet 固定 x64-mingw-dynamic",
  "配置 Debug / Release 两套构建：Debug 关闭优化保留断言，Release 开 -O2 且保留 NDEBUG 断言开关可回退",
  "写 vcpkg.json：name/version/builtin-baseline=aae277ac，第一版依赖先只放 fmt、gtest、benchmark、protobuf、grpc、flatbuffers（按需裁剪，禁止一次性全上）",
  "写 .gitignore：build/ out/ vcpkg_installed/ *.o *.obj *.exe *.pdb .vs/ .idea/ .cache/",
  "写 .github/workflows/build.yml：ubuntu-latest + windows-latest 两档，仅做 configure+build+ctest，**不作为可信验收依据**（可信验收=本地）",
  "建立 task/ 目录与脚本入口 scripts/verify/_common.sh、scripts/task-done.sh（此时还是空工程，脚本只做占位与占位校验）",
 ],
 unit="无业务代码，只验证构建系统：cmake configure 成功、Debug 与 Release 各成功构建一次、`ctest` 可执行且返回「No tests were found」而非崩溃",
 integ="git clone 到临时目录后按 README 三条命令可复现构建；CI workflow 语法通过 actionlint（本地可选）",
 bench="无",
 fail="故意写入一个语法错误源文件，验证构建**会失败**（防止 CI 假绿）；删除 vcpkg toolchain 参数，验证 configure 报错而非静默跳过",
 accept=[
  "`cmake -S . -B build/Debug -DCMAKE_BUILD_TYPE=Debug` 与 Release 均 configure 成功",
  "Debug / Release 两套均 `cmake --build` 成功，退出码 0",
  "`ctest --test-dir build/Release` 可执行且不报错",
  "README.md / LICENSE / PROJECT_REQUIREMENTS.md / ARCHITECTURE.md / DEVELOPMENT.md 五份文件存在且非空",
  "PROJECT_REQUIREMENTS.md 首屏含「冻结」声明与 RFC 变更流程",
  "根目录下**不存在**任何游戏功能代码（src 中无 combat/scene/aoi 等字样），用 grep 验证",
  ".gitignore 生效：`git status --short` 不出现 build/、vcpkg_installed/",
  "CI workflow 在 GitHub Actions 上至少触发一次并输出构建日志（失败不影响本地验收结论，但必须记录结论到 docs/ci-status.md）",
 ],
 forbid=[
  "禁止实现任何游戏功能（战斗/移动/AOI/网络等）",
  "禁止在 vcpkg.json 里一次性声明全部依赖，按需增量添加",
  "禁止把 CI 结果当作验收通过依据",
  "禁止提交 build/、vcpkg_installed/ 等构建产物",
  "禁止在没有 LICENSE 的情况下初始化仓库",
 ],
 perf="无性能目标。构建耗时基线记录到 docs/build-baseline.md（冷构建/增量构建各一次），作为后续编译性能回归参照。",
 deliver=["README.md", "LICENSE", "PROJECT_REQUIREMENTS.md", "ARCHITECTURE.md", "DEVELOPMENT.md",
          "CMakeLists.txt", "vcpkg.json", ".gitignore", ".gitattributes", "cmake/*.cmake",
          ".github/workflows/build.yml", "docs/build-baseline.md", "docs/ci-status.md"],
 ctest="Core", both_build=True,
 artifacts=["README.md", "PROJECT_REQUIREMENTS.md", "ARCHITECTURE.md", "DEVELOPMENT.md", "CMakeLists.txt", "vcpkg.json"],
 scan=[("engine", r"\bstd::cout\s*<<"), ("server", r"\bstd::cout\s*<<"), ("engine", r"\bprintf\s*\(")],
 ctype="build",
),
# ------------------------------------------------------------------ 001
dict(
 id="TASK-001", name="Core Error / Result 系统", phase="Phase 0 · 工程基础",
 objective="统一整个项目的错误处理：提供 Result<T> / ErrorCode / Error 三件套与 9 个标准错误码，服务端与客户端共用同一套定义，禁止任何模块自行设计冲突的错误体系。",
 deps="TASK-000",
 module="engine/core",
 owner=OWNER,
 inp="TASK-000 的空工程骨架；PROJECT_REQUIREMENTS.md 第 37 节「错误处理」",
 out="engine/core 模块骨架 + error/result 头文件 + 单元测试 + 交付文档",
 iface="""```cpp
namespace mmo::core {
enum class ErrorCode : int16_t {
  OK = 0, INVALID_ARGUMENT = 1, NOT_FOUND = 2, TIMEOUT = 3, BUSY = 4,
  VERSION_CONFLICT = 5, UNAUTHORIZED = 6, RATE_LIMITED = 7, INTERNAL_ERROR = 8,
};
class Error final {                       // 值语义，无异常
 public:
  Error(ErrorCode c, std::string msg, std::string domain = "core");
  ErrorCode Code() const noexcept;
  std::string_view Message() const noexcept;
  std::string_view Domain() const noexcept;   // 产生错误的模块域，便于定位
  std::string ToString() const;               // "core/NOT_FOUND: player 42 not exist"
  bool IsRetryable() const noexcept;          // TIMEOUT / BUSY / RATE_LIMITED = true
};
template <typename T> class [[nodiscard]] Result {
 public:
  static Result Ok(T v); static Result Fail(Error e);
  bool HasValue() const noexcept; explicit operator bool() const noexcept;
  const T& Value() const&;                    // 无值时终止（断言 + 日志）
  const Error& Err() const&;
  T ValueOr(T fallback) const;
  // monadic：链式组合，避免层层 if
  template <typename F> auto AndThen(F&& f);
  template <typename F> auto Map(F&& f);
};
template <> class [[nodiscard]] Result<void>;   // 特化：只关心成功/失败
const char* ToString(ErrorCode) noexcept;       // 与数值双向映射，禁止重复定义
}
```""",
 data="""| 类型 | 字段 | 说明 |
|---|---|---|
| ErrorCode | int16_t 枚举 | 9 个标准码，值固定，序列化后跨进程稳定 |
| Error | code / message / domain | 值语义，可拷贝，禁止抛异常 |
| Result&lt;T&gt; | variant&lt;T, Error&gt; | `[[nodiscard]]`，禁止丢弃返回值 |""",
 thread="全线程安全：`Error` 不可变，`Result<T>` 只读。禁止内部使用全局可变状态或静态缓存。",
 hot="NO（但 Result 必须零异常、零动态分配失败路径，供热路径安全使用）", io="NO", rpc="NO", persist="NO",
 files=["engine/core/include/mmo/core/error/", "engine/core/src/error/", "engine/core/tests/", "engine/core/docs/"],
 steps=[
  "建模块目录 engine/core，按统一模板建 include/mmo/core、src、tests、docs、CMakeLists.txt",
  "实现 error_code.h：ErrorCode 枚举 + ToString/FromString 双向映射 + IsRetryable 判定",
  "实现 error.h/.cpp：Error 值类型，message 用 SSO 优化（≤32 字节不分配堆内存）",
  "实现 result.h：模板 Result<T> 与 Result<void> 特化，内部用 std::variant，加 `[[nodiscard]]`",
  "实现 AndThen/Map 链式操作，保证失败短路且不产生额外分配",
  "实现 `MMO_TRY(expr)` 宏（等价于 Rust 的 ?），失败时提前 return Err",
  "定义模块错误域常量（core / net / scene / combat / data / economy / lua），Error.domain 只允许取这些值",
  "写单元测试：每个错误码的 ToString/FromString 往返、Result 成功/失败路径、Value() 越界的断言行为、链式 AndThen 短路、Result<void> 语义、零分配路径验证（自定义 allocator 计数）",
  "写 docs/INTERFACE.md 与 docs/README.md，明确「禁止新增顶层错误码，扩展请用 domain 细分」",
  "在根 CMakeLists.txt 中 add_subdirectory(engine/core)，target 名 `mmo::core_error`",
 ],
 unit="ErrorCode 往返映射全覆盖；Result 成功/失败/移动/拷贝语义；`[[nodiscard]]` 编译告警验证（丢弃返回值必须告警）；Result<void>；MMO_TRY 短路；失败路径零堆分配（allocator 计数器断言 alloc==0）",
 integ="在一个假模块（tests/fixture）中用 Result 串联三层调用（repo→service→handler），验证错误码与 domain 原样透传不被改写；同一 Error 序列化后跨「进程边界模拟」反序列化一致",
 bench="engine/core/tests/error_bench：1e7 次 Result 构造+析构、1e7 次失败路径传递；输出 `result_ns_per_op=` 与 `alloc_per_fail=`",
 fail="错误码越界（FromString(999)）返回 nullopt 而非崩溃；Error message 超长（4KB）不截断溢出；Result 在移动后被访问时触发断言（Debug）而非 UB",
 accept=[
  "9 个错误码全部实现，ToString/FromString 双向一致（单测覆盖）",
  "Result<T> 与 Result<void> 均带 `[[nodiscard]]`，丢弃返回值在 -Wall -Wextra 下产生告警",
  "单元测试全部通过（ctest -R Core_Error）",
  "benchmark 输出 `result_ns_per_op` 且失败路径 `alloc_per_fail=0`",
  "grep 全仓：除 engine/core 外**不存在**第二个 enum class ErrorCode 定义",
  "Debug / Release 双构建通过",
  "engine/core/docs/README.md 与 INTERFACE.md 存在且含使用示例",
 ],
 forbid=[
  "禁止使用 C++ 异常作为业务错误传播机制（第三方库异常须在边界转换为 Error）",
  "禁止任何模块自行定义新的顶层 ErrorCode 枚举",
  "禁止在 Error 构造中做 IO、加锁或分配大对象",
  "禁止 Result 失败路径产生堆分配",
  "禁止用 int / bool 返回值代替 Result",
 ],
 perf="Result 构造+析构 < 5ns/op；失败路径堆分配次数 = 0；`ToString()` 单次 < 100ns（无格式化到 std::string 的额外拷贝）。数字由 benchmark 实测写入 docs/PERFORMANCE.md。",
 deliver=["engine/core/include/mmo/core/error/error_code.h", "engine/core/include/mmo/core/error/error.h",
          "engine/core/include/mmo/core/error/result.h", "engine/core/src/error/*.cpp",
          "engine/core/tests/*", "engine/core/docs/README.md", "engine/core/docs/INTERFACE.md",
          "engine/core/docs/PERFORMANCE.md"],
 ctest="Core_Error", both_build=True,
 bench_bins=[("bin/error_bench", "--iterations 10000000")],
 metrics=[("bench/core_error.txt", "result_ns_per_op", "le", "5"),
          ("bench/core_error.txt", "alloc_per_fail", "le", "0")],
 artifacts=["engine/core/include/mmo/core/error/result.h", "engine/core/docs/INTERFACE.md"],
 scan=[("engine/core/src/error", r"\bthrow\s+"), ("engine/core/include/mmo/core/error", r"\bthrow\s+")],
),
# ------------------------------------------------------------------ 002
dict(
 id="TASK-002", name="Core Logger / Trace", phase="Phase 0 · 工程基础",
 objective="实现统一 Logger 与 TraceID 传播机制，使同一请求的全部日志可通过 TraceID 串起来；日志字段固定为 Timestamp/Level/Service/Module/TraceID/RequestID/PlayerID/SceneID/Message。",
 deps="TASK-001",
 module="engine/core",
 owner=OWNER,
 inp="TASK-001 的 Result/Error（Logger 初始化失败用 Error 返回）；PROJECT_REQUIREMENTS.md 第 38 节",
 out="Logger / LogContext / TraceID 实现 + 多线程压测 + TraceID 串联验证脚本",
 iface="""```cpp
namespace mmo::core {
enum class LogLevel : uint8_t { Trace=0, Debug=1, Info=2, Warn=3, Error=4, Fatal=5 };
struct LogContext {                    // 线程局部，随调用链自动传播
  TraceID   trace_id{0};
  RequestID request_id{0};
  PlayerID  player_id{kInvalidPlayerId};
  SceneID   scene_id{kInvalidSceneId};
  std::string_view module;             // 静态字符串，不允许运行时拼接
};
class ScopedLogContext final {         // RAII 覆盖/恢复，异常安全
 public:
  explicit ScopedLogContext(LogContext patch);
  ~ScopedLogContext();
};
class ILogSink { public: virtual ~ILogSink() = default;
  virtual void Write(const LogRecord&) noexcept = 0; };
class Logger final {
 public:
  static Result<void> Init(LoggerConfig cfg);
  static void RegisterSink(std::shared_ptr<ILogSink>);
  static void SetLevel(LogLevel);
  static bool ShouldLog(LogLevel) noexcept;
  static void Write(LogLevel, const LogContext&, std::string_view fmt, ...); // fmtlib 格式化
  static void Flush() noexcept;                 // 优雅退出时调用
};
}
#define MMO_LOG(level, fmt, ...)  do { if (::mmo::core::Logger::ShouldLog(level)) \\
  ::mmo::core::Logger::Write(level, ::mmo::core::CurrentLogContext(), fmt, ##__VA_ARGS__); } while(0)
```""",
 data="""**LogRecord（内部）**

| 字段 | 类型 | 说明 |
|---|---|---|
| timestamp_ns | int64 | 单调时钟 + 启动时刻换算出的墙钟，纳秒 |
| level | LogLevel | Trace..Fatal |
| service | string_view | gateway / gamenode / dataservice / control |
| module | string_view | 静态字符串，编译期常量 |
| trace_id | uint64 | 全链路唯一，跨进程透传 |
| request_id | uint64 | 单次 Command/Query 唯一 |
| player_id / scene_id | uint64 | 业务维度，无值填 kInvalid |
| thread_id | uint32 | 便于定位线程模型问题 |
| message | string | 格式化后文本 |

**TraceID**：`uint64`，由 `(node_id << 48) | (timestamp_low << 16) | counter` 生成，禁止用随机数（要可排序）。""",
 thread="Logger 为**无锁 MPSC 环形缓冲 + 独立后台刷盘线程**；业务线程只做格式化入队，不碰文件 IO。LogContext 存 thread_local，跨线程提交任务时必须显式拷贝传递（提供 `WithContext` 包装器）。",
 hot="NO（但必须支持 `ShouldLog` 编译期/运行期短路，热路径关闭日志时开销 ≈ 单次分支）", io="YES（后台线程写文件，业务线程不阻塞）", rpc="NO", persist="NO",
 files=["engine/core/include/mmo/core/log/", "engine/core/src/log/", "engine/core/tests/", "engine/core/docs/", "tools/logtrace/"],
 steps=[
  "实现 log_level.h：LogLevel 与 ToString，支持从字符串配置（ConfigManager 后续接入，本任务先用 LoggerConfig 结构体）",
  "实现 trace_id.h：TraceID/RequestID 生成器（NodeID + 单调时钟 + 原子计数），提供 `NewTraceID()` / `DeriveRequestID(trace)`",
  "实现 log_context.h/.cpp：thread_local 当前上下文栈，ScopedLogContext RAII 覆盖恢复，提供 `WithContext(fn)` 用于跨线程携带",
  "实现 log_record.h：LogRecord POD，字段固定九项 + thread_id",
  "实现 async_ring_buffer.h：无锁 MPSC 环形队列（固定容量，满时按策略丢弃并计数，禁止阻塞业务线程）",
  "实现 logger.h/.cpp：Logger 静态门面 + ShouldLog 短路 + fmtlib 格式化（编译期校验格式串）",
  "实现两个 sink：ConsoleSink（带颜色，开发用）、RotatingFileSink（按大小滚动，保留 N 个，后台线程写）",
  "实现结构化 JSON 输出模式（配置开关），字段与 LogRecord 一一对应",
  "写 tools/logtrace/parse_trace.py：给定 TraceID，从日志文件中抽出全部相关行并按时间排序输出",
  "写测试：多线程（8 线程 × 每线程 10 万条）压测不丢不乱、字段完整、无交叉串行；TraceID 串联测试；关闭日志时开销测试（对比空循环）",
  "写 docs/README.md（字段说明 + 使用规范）与 docs/TEST.md",
 ],
 unit="LogLevel 解析；TraceID 唯一性（100 万次无碰撞）与单调递增性；ScopedLogContext 嵌套恢复正确；环形队列单生产者单消费者顺序一致；格式化非法格式串时编译期报错",
 integ="模拟一次跨线程请求：主线程设 TraceID → 投递到 WorkerThread（WithContext 携带）→ 子线程打日志 → 用 parse_trace.py 按 TraceID 检索，必须拿到全部 3 条日志且字段一致；RotatingFileSink 触发滚动后文件数量与命名正确",
 bench="bin/log_bench：8 线程 × 100k 条，`log_ns_per_msg=`；关闭日志路径 `disabled_ns_per_call=`；丢包计数 `dropped=`",
 fail="环形队列打满时：不阻塞、不崩溃、丢弃计数单调增加并产生一条 Warn；后台线程被卡住时业务线程仍能继续写入；磁盘不可写（指向只读目录）时 Logger::Init 返回 Error 而非崩溃；Fatal 级别写入后立即 Flush",
 accept=[
  "九项字段全部出现在每条日志记录中（JSON 模式下 key 齐全，单测断言）",
  "8 线程 × 10 万条压测无死锁、无交叉错乱，单线程内顺序严格递增",
  "给定 TraceID，`python tools/logtrace/parse_trace.py <trace>` 能完整串起跨线程日志",
  "关闭日志时单次 `MMO_LOG` 调用开销 < 5ns（benchmark 实测）",
  "环形队列满时 `dropped` 计数正确且业务线程不阻塞",
  "全仓 grep 无 `std::cout` / `printf` 直接输出（红线扫描）",
  "Debug / Release 双构建通过，ctest -R Core_Log 全绿",
 ],
 forbid=[
  "禁止在业务线程做文件 IO 或加锁写盘",
  "禁止使用 std::cout / printf / std::cerr 直接输出",
  "禁止用随机数生成 TraceID",
  "禁止在热路径拼接字符串后再判断日志级别（必须先 ShouldLog）",
  "禁止日志内容包含明文口令、令牌、完整身份证/银行卡等敏感数据",
  "禁止日志队列满时阻塞业务线程",
 ],
 perf="开启日志：< 800ns/条（含格式化，8 线程并发）；关闭日志：< 5ns/次调用；单条日志堆分配次数 = 0（复用 per-thread scratch buffer）；丢弃率在 8×100k 压测下 < 0.1%。",
 deliver=["engine/core/include/mmo/core/log/logger.h", "engine/core/include/mmo/core/log/log_context.h",
          "engine/core/include/mmo/core/log/trace_id.h", "engine/core/src/log/*.cpp",
          "engine/core/tests/*", "tools/logtrace/parse_trace.py",
          "engine/core/docs/README.md", "engine/core/docs/PERFORMANCE.md"],
 ctest="Core_Log", both_build=True,
 bench_bins=[("bin/log_bench", "--threads 8 --per-thread 100000")],
 metrics=[("bench/core_log.txt", "disabled_ns_per_call", "lt", "5"),
          ("bench/core_log.txt", "log_ns_per_msg", "le", "800")],
 artifacts=["engine/core/include/mmo/core/log/logger.h", "tools/logtrace/parse_trace.py"],
 scan=[("engine/core/src", r"\bstd::cout\s*<<"), ("engine/core/src", r"\bprintf\s*\(")],
),
# ------------------------------------------------------------------ 003
dict(
 id="TASK-003", name="Core Time / UUID / Config", phase="Phase 0 · 工程基础",
 objective="实现 Time（MonotonicClock / WallClock）、UUID 生成与 ConfigManager。**特别要求：游戏 Tick 计时只能基于 MonotonicClock，禁止使用系统墙钟作为唯一计时依据。**",
 deps="TASK-000",
 module="engine/core",
 owner=OWNER,
 inp="TASK-000 工程骨架；PROJECT_REQUIREMENTS.md 第 13 节 Tick 模型",
 out="time / uuid / config 三个子模块 + 单调时钟正确性测试 + 配置热加载测试",
 iface="""```cpp
namespace mmo::core {
// ---- 时钟 ----
class MonotonicClock {                       // 不受系统时间调整影响
 public:
  static SteadyNs   Now() noexcept;          // std::chrono::steady_clock 语义
  static SteadyTime Point() noexcept;
};
class WallClock {                            // 仅用于展示、日志时间戳、跨机对齐
 public:
  static int64_t UnixNanos() noexcept;
  static int64_t UnixMillis() noexcept;
};
// Tick 计时唯一入口：禁止业务直接调用 WallClock
class TickClock final {
 public:
  explicit TickClock(uint32_t hz) noexcept;  // 20Hz -> 50ms
  DurationMs TickInterval() const noexcept;
  SteadyTime NextTickDeadline(SteadyTime prev) const noexcept;
  uint32_t   CatchUpSteps(SteadyTime now, SteadyTime prev) const noexcept;  // 限幅，防死亡螺旋
};
// ---- UUID ----
class Uuid { public:
  static Uuid NewV4();                       // 随机，用于 MessageID / TransactionID
  static Uuid NewV7();                       // 时间有序，用于数据库主键
  std::string ToString() const; static Result<Uuid> Parse(std::string_view);
  std::array<uint8_t,16> bytes; };
// ---- Config ----
class ConfigManager { public:
  static Result<void> LoadFile(std::string_view path);   // JSON / YAML
  static Result<void> LoadDir(std::string_view dir);
  template <typename T> static Result<T> Get(std::string_view key);
  static Result<void> Set(std::string_view key, std::string value);  // 运行期覆盖，仅测试用
  static uint64_t Version() noexcept;                                 // 每次变更 +1
  static Result<void> Reload();                                       // 原子替换快照
};
}
```""",
 data="""| 类型 | 表示 | 说明 |
|---|---|---|
| SteadyNs | int64 纳秒 | 单调时钟差值，永不受 NTP/手动改时间影响 |
| SteadyTime | steady_clock::time_point | Tick 调度基准 |
| Uuid | 16 字节 | V4（随机）/ V7（时间有序，DB 主键） |
| ConfigSnapshot | 不可变键值树 | 版本化，Reload 时原子替换，读者无锁 |""",
 thread="TickClock 只在 SimulationThread 使用，不加锁。ConfigManager 用 `shared_ptr<const Snapshot>` + 原子替换，读路径无锁；Reload 由 ControlService/运维线程触发。UUID 生成器用 thread_local 状态，**无全局锁**。",
 hot="YES（TickClock 位于 Tick 循环入口，每次 Tick 调用一次）", io="YES（ConfigManager 读文件，只在加载/热更时）", rpc="NO", persist="NO",
 files=["engine/core/include/mmo/core/time/", "engine/core/include/mmo/core/uuid/", "engine/core/include/mmo/core/config/", "engine/core/src/{time,uuid,config}/", "engine/core/tests/", "config/"],
 steps=[
  "实现 time/clock.h：MonotonicClock / WallClock 薄封装，明确注释「Tick 禁止用 WallClock」",
  "实现 time/tick_clock.h：固定 Hz（默认 20）、计算 deadline、CatchUpSteps 限幅（单帧最多补 3 个 Tick，防止死亡螺旋）",
  "实现 time/timer.h：基于单调时钟的一次性/周期定时器接口（为 TASK-004 Scheduler 预留，本任务只定义接口不实现调度）",
  "实现 uuid.h/.cpp：V4（OS 熵源）与 V7（时间前缀 + 随机），提供 ToString/Parse，禁止依赖 boost",
  "实现 config/config_manager.h/.cpp：JSON 解析（用已引入的第三方 JSON 库或自研极简解析），版本化快照、原子替换、Get<T> 模板特化",
  "实现配置校验：`Get` 缺失 key 返回 ErrorCode::NOT_FOUND 并带 key 名；类型不匹配返回 INVALID_ARGUMENT",
  "实现 `Reload()`：加载失败时保留旧快照并返回 Error，禁止半替换",
  "建立 config/ 目录：放 app.json（service 名、log 级别）、tick.json（hz=20）、network.json 占位",
  "写测试：单调性验证（在测试内无法改系统时间，改用注入时钟接口验证 WallClock 回拨时 TickClock 不受影响）；Tick 累计漂移测试（跑 10000 次理论 500s，误差 < 10ms）；UUID 唯一性 100 万次；配置热更原子性（Reload 期间并发读不崩溃且读到完整快照）",
  "写 docs/INTERFACE.md，明确「Tick 计时红线」",
 ],
 unit="MonotonicClock 单调不回退（100 万次采样）；TickClock 20Hz 的 interval=50ms、CatchUpSteps 限幅生效；UUID V4/V7 格式正确、100 万次无碰撞、Parse/ToString 往返；Config Get/Set/Reload、错误码正确、快照版本递增",
 integ="ConfigManager 在 Reload 过程中由 4 个读线程并发 Get，验证不会读到半更新状态；TickClock 驱动一个假 Tick 循环跑 10 秒，实测 Tick 次数 = 200 ± 2 且无累积漂移",
 bench="bin/time_bench：`monotonic_ns_per_call=`（目标 < 25ns）、`uuid_v4_ns=` / `uuid_v7_ns=`（目标 < 100ns）、`config_get_ns=`（目标 < 50ns，读路径无锁）",
 fail="配置文件损坏（非法 JSON）：LoadFile 返回 INVALID_ARGUMENT，旧快照保持不变；配置目录被删除：Reload 返回 NOT_FOUND 且服务继续用旧配置；注入时钟回拨 5 秒：TickClock 产生的 deadline 序列仍单调；UUID 熵源失败：返回 INTERNAL_ERROR 而非生成弱 UUID",
 accept=[
  "全仓 grep：Tick 相关代码路径中**不存在**对 WallClock / system_clock 的调用（红线扫描）",
  "TickClock 跑 10000 次，累计误差 < 10ms，无漂移",
  "UUID 100 万次无碰撞，V7 可按时间排序",
  "ConfigManager 热更期间并发读安全（TSan 或压测 100 万次读无异常）",
  "benchmark 三项指标达标",
  "config/ 下至少 3 份配置可被正确加载并 Get 到值",
  "Debug / Release 双构建通过，ctest -R Core_Time 全绿",
 ],
 forbid=[
  "禁止用系统墙钟（std::chrono::system_clock / time(nullptr)）驱动游戏 Tick",
  "禁止在 Tick 循环里读取配置文件",
  "禁止 UUID 使用全局锁或共享随机引擎",
  "禁止 ConfigManager 返回裸指针/引用给调用方长期持有",
  "禁止配置加载失败时留下半更新状态",
 ],
 perf="MonotonicClock::Now < 25ns/次；UUID V4/V7 < 100ns/个；Config Get < 50ns/次（读路径无锁无分配）；TickClock 10 分钟累计漂移 < 50ms。",
 deliver=["engine/core/include/mmo/core/time/clock.h", "engine/core/include/mmo/core/time/tick_clock.h",
          "engine/core/include/mmo/core/uuid/uuid.h", "engine/core/include/mmo/core/config/config_manager.h",
          "engine/core/src/{time,uuid,config}/*.cpp", "engine/core/tests/*",
          "config/app.json", "config/tick.json", "config/network.json", "engine/core/docs/INTERFACE.md"],
 ctest="Core_Time", both_build=True,
 bench_bins=[("bin/time_bench", "--samples 1000000")],
 metrics=[("bench/core_time.txt", "monotonic_ns_per_call", "le", "25"),
          ("bench/core_time.txt", "config_get_ns", "le", "50")],
 artifacts=["engine/core/include/mmo/core/time/tick_clock.h", "config/tick.json"],
 scan=[("engine/core/src/time", r"std::chrono::system_clock"), ("engine/core/include/mmo/core/time", r"std::chrono::system_clock")],
),
# ------------------------------------------------------------------ 004
dict(
 id="TASK-004", name="Core Memory / Thread / Scheduler", phase="Phase 0 · 工程基础",
 objective="建立线程模型与执行骨架：Thread 抽象、Task 提交、Scheduler 定时任务、ObjectPool / MemoryPool / Arena 内存设施，并固定四类线程（Network / Simulation / Worker / Persistence）。",
 deps="TASK-001, TASK-003",
 module="engine/core",
 owner=OWNER,
 inp="TASK-001 Error/Result；TASK-003 MonotonicClock（定时器基准）",
 out="thread / scheduler / memory 三个子模块 + 并发压测 + ObjectPool & Scheduler benchmark",
 iface="""```cpp
namespace mmo::core {
enum class ThreadRole : uint8_t { Network, Simulation, Worker, Persistence };
class Thread { public:
  struct Config { ThreadRole role; std::string name; size_t queue_capacity{4096}; };
  static Result<std::unique_ptr<Thread>> Create(Config, std::function<void()> on_start = {});
  Result<void> Post(TaskFn);                 // 非阻塞入队，队列满返回 BUSY
  Result<void> PostBlocking(TaskFn);
  void RequestStop() noexcept; void Join();
  ThreadRole Role() const noexcept; size_t Pending() const noexcept;   // 队列深度指标
};
class Scheduler { public:                    // 基于 TASK-003 TickClock 的单调定时器
  using TimerId = uint64_t;
  Result<TimerId> ScheduleAfter(DurationMs, TaskFn);
  Result<TimerId> ScheduleAt(SteadyTime, TaskFn);
  Result<TimerId> ScheduleEvery(DurationMs, TaskFn);   // 周期任务，用于 Buff Tick
  Result<void>    Cancel(TimerId);
  size_t ReadyCount() const noexcept;        // 到期待执行数量（指标）
  Result<size_t>  Tick(SteadyTime now);      // 由宿主线程驱动，**不自带线程**
};
template <typename T, size_t Chunk = 4096> class ObjectPool {
 public:
  explicit ObjectPool(size_t prewarm = 0);
  T* Acquire(); void Release(T*) noexcept;
  size_t Capacity() const noexcept; size_t InUse() const noexcept;
};
class MemoryPool { public:                   // 定长块分配，无全局锁
  void* Allocate(size_t bytes); void Deallocate(void*, size_t) noexcept;
  size_t UsedBytes() const noexcept; size_t ChunkSize() const noexcept; };
class Arena { public:                        // 帧内分配，整块重置
  explicit Arena(size_t bytes); void* Push(size_t bytes, size_t align = 8);
  void Reset() noexcept; size_t UsedBytes() const noexcept; };
}
```""",
 data="""| 类型 | 说明 |
|---|---|
| ThreadRole | Network / Simulation / Worker / Persistence，四类角色固定，禁止新增第五类不打招呼 |
| TaskFn | `std::function<void()>` 的替代品：小对象优化的 `fu2::function` 或自定义 32 字节内联缓冲，**避免每次提交都堆分配** |
| TimerId | 单调递增 uint64，Cancel 幂等 |
| ObjectPool&lt;T&gt; | 自由链表 + 分块扩容，Release 后对象析构但内存不归还 OS |""",
 thread="Scheduler **不自带线程**，必须由宿主线程调用 `Tick(now)`（SimulationThread 驱动游戏定时器，WorkerThread 驱动后台定时器），从根本上避免「每个 Buff 一个 Timer 线程」。ObjectPool/MemoryPool 用 thread_local 缓存 + 全局后备，热路径无锁。",
 hot="YES（ObjectPool 与 Scheduler 位于 Tick 热路径）", io="NO", rpc="NO", persist="NO",
 files=["engine/core/include/mmo/core/thread/", "engine/core/include/mmo/core/sched/", "engine/core/include/mmo/core/memory/", "engine/core/src/{thread,sched,memory}/", "engine/core/tests/", "engine/core/benchmark/"],
 steps=[
  "实现 thread/task.h：TaskFn 类型（小对象优化，32 字节内联，避免堆分配）",
  "实现 thread/thread.h/.cpp：Thread 封装（角色命名、队列容量、优雅停止、Pending 指标）",
  "实现 thread/mpmc_queue.h：无锁 MPMC 队列（Vyukov 风格），单测验证多生产者多消费者正确性",
  "实现 sched/scheduler.h/.cpp：四级时间轮或最小堆（选最小堆，实现简单且 Cancel 高效）+ Tick(now) 驱动 + 到期任务批量出队",
  "实现 Cancel 幂等与已取消任务的资源回收（禁止内存泄漏）",
  "实现 memory/object_pool.h：分块自由链表，支持 prewarm，提供 InUse/Capacity 指标",
  "实现 memory/memory_pool.h：定长块池，thread_local free list + 全局后备，跨线程归还有界",
  "实现 memory/arena.h：bump allocator，Reset 整块回收，用于 Tick 帧内临时对象",
  "写并发测试：4 生产者 4 消费者各 10 万任务，验证无丢失、无重复、无死锁；Scheduler 1000 个周期定时器精度测试",
  "写 benchmark：ObjectPool acquire/release、MemoryPool alloc/free、Scheduler 1 万定时器 Tick 耗时",
  "写 docs/DEPENDENCY.md 与 docs/PERFORMANCE.md，明确 Scheduler 的线程归属红线",
 ],
 unit="MPMC 队列单/多生产者消费者正确性与顺序；Thread 启停与优雅 Join；Scheduler 一次性/周期/取消/幂等 Cancel；ObjectPool Acquire/Release 不泄漏（ASan 下跑）；MemoryPool 大小对齐；Arena Reset 后可复用",
 integ="四类线程各起一个实例，Network 投递任务到 Worker，Worker 回投到 Simulation，跑 10 秒无死锁无丢任务；Scheduler 挂在 SimulationThread 上驱动 500 个 20Hz 周期任务，实测每秒触发 10000 ± 50 次",
 bench="bin/sched_bench：`sched_tick_us_10k_timers=`；bin/mem_bench：`pool_acquire_release_ns=` / `arena_push_ns=` / `mempool_alloc_ns=`",
 fail="任务队列满：Post 返回 BUSY 而非阻塞或丢任务；Scheduler 到期任务抛错：捕获记录并继续，不影响后续定时器；ObjectPool 耗尽：按配置扩容或返回 nullptr（禁止崩溃）；线程 Join 超时：记录告警并上报指标；跨线程归还 MemoryPool 块：不崩溃、不双释放（ASan 验证）",
 accept=[
  "Scheduler **不创建任何线程**（grep 验证：`std::thread` 不出现在 sched/ 目录）",
  "4×4 并发 10 万任务无丢失、无重复、无死锁",
  "Scheduler 挂 500 个 20Hz 周期任务，10 秒内触发次数误差 < 1%",
  "ObjectPool / MemoryPool / Arena 在 ASan 下无泄漏、无越界",
  "benchmark 四项指标达标并写入 docs/PERFORMANCE.md",
  "每类线程暴露 Pending/ReadyCount 指标（供 TASK-039 指标采集）",
  "Debug / Release 双构建通过，ctest -R Core_Thread 全绿",
 ],
 forbid=[
  "禁止 Scheduler 自带线程（必须宿主驱动）",
  "禁止为每个 Buff / 每个定时任务创建一个线程或一个 OS timer",
  "禁止在热路径使用 std::function 造成堆分配（用 TaskFn）",
  "禁止在 Tick 内做阻塞式 PostBlocking",
  "禁止使用全局锁保护 ObjectPool 的热路径分配",
  "禁止在 Tick 内 new/delete 大对象（应走 Pool / Arena）",
 ],
 perf="ObjectPool acquire+release < 20ns；MemoryPool alloc < 15ns；Arena push < 3ns；Scheduler Tick（1 万定时器）< 200us；任务投递 < 100ns。",
 deliver=["engine/core/include/mmo/core/thread/thread.h", "engine/core/include/mmo/core/thread/mpmc_queue.h",
          "engine/core/include/mmo/core/sched/scheduler.h", "engine/core/include/mmo/core/memory/object_pool.h",
          "engine/core/include/mmo/core/memory/memory_pool.h", "engine/core/include/mmo/core/memory/arena.h",
          "engine/core/src/{thread,sched,memory}/*.cpp", "engine/core/tests/*", "engine/core/benchmark/*",
          "engine/core/docs/DEPENDENCY.md", "engine/core/docs/PERFORMANCE.md"],
 ctest="Core_Thread", both_build=True,
 bench_bins=[("bin/sched_bench", "--timers 10000 --ticks 1000"),
             ("bin/mem_bench", "--ops 10000000")],
 metrics=[("bench/core_sched.txt", "sched_tick_us_10k_timers", "le", "200"),
          ("bench/core_mem.txt", "pool_acquire_release_ns", "le", "20")],
 artifacts=["engine/core/include/mmo/core/sched/scheduler.h", "engine/core/include/mmo/core/memory/object_pool.h"],
 scan=[("engine/core/src/sched", r"std::thread"), ("engine/core/include/mmo/core/sched", r"std::thread")],
),
# ------------------------------------------------------------------ 005
dict(
 id="TASK-005", name="Protocol Schema（Protobuf + FlatBuffers）", phase="Phase 1 · 统一通信",
 objective="定义全项目唯一的消息契约：MessageEnvelope / Command / Query / Event 的 Protobuf 定义，以及高频游戏数据的 FlatBuffers 定义。C++ 侧可 Encode / Decode / Validate / Version Check。这是后续所有跨进程与客户端通信的唯一真相源。",
 deps="TASK-001",
 module="protocol",
 owner=OWNER,
 inp="PROJECT_REQUIREMENTS.md 第 36 节 Message Envelope；TASK-001 ErrorCode（协议错误映射）",
 out="protocol/proto/*.proto、protocol/flatbuffers/*.fbs、生成代码接入 CMake、编解码与版本校验库 + 测试",
 iface="""```cpp
namespace mmo::protocol {
enum class MessageType : uint8_t { Command = 1, Query = 2, Event = 3, Response = 4, Heartbeat = 5 };
struct EnvelopeView {                      // 解码后的零拷贝视图
  MessageId   message_id; MessageType type; uint32_t version;
  std::string_view source; int64_t timestamp_ms;
  TraceID trace_id; RequestID request_id;
  std::string_view payload;
};
class ICodec { public:
  virtual ~ICodec() = default;
  virtual Result<Buffer> Encode(const EnvelopeView&) = 0;
  virtual Result<EnvelopeView> Decode(std::string_view bytes) = 0;
};
class ProtobufCodec final : public ICodec {};   // 低频：管理面 / 数据面
class FlatbufCodec final : public ICodec {};    // 高频：移动 / AOI / 战斗
class EnvelopeValidator { public:
  static Result<void> Validate(const EnvelopeView&, uint32_t expected_version);
};
}
```""",
 data="""**MessageEnvelope（proto）**

| 字段 | 类型 | 必填 | 说明 |
|---|---|---|---|
| message_id | fixed64 | Y | 全链路唯一（= Uuid V4 前 8 字节或雪花） |
| message_type | enum | Y | Command/Query/Event/Response/Heartbeat |
| version | uint32 | Y | 协议版本，用于兼容校验 |
| source | string | Y | 产生者标识 `gateway-01` / `gamenode-07` |
| timestamp_ms | int64 | Y | 产生时刻（墙钟，仅用于审计） |
| trace_id | fixed64 | Y | 全链路追踪 |
| request_id | fixed64 | Y | 单次请求唯一 |
| payload | bytes | Y | 具体 Command/Query/Event 序列化结果 |
| transaction_id | fixed64 | N | 仅经济类消息 |
| idempotency_key | string | N | 仅经济类消息 |

**版本规则**：`version` 为主版本号，破坏性变更才 +1；解码端遇到不支持版本返回 `VERSION_CONFLICT`，禁止静默降级。""",
 thread="Codec 无状态、线程安全，可在任意线程使用。生成代码只读，禁止手改 `*.pb.h` / `*_generated.h`。",
 hot="YES（FlatBuffers 编解码位于网络与复制热路径）", io="NO", rpc="NO", persist="NO",
 files=["protocol/proto/", "protocol/flatbuffers/", "protocol/include/mmo/protocol/", "protocol/src/", "protocol/tests/", "protocol/docs/", "protocol/CMakeLists.txt"],
 steps=[
  "定义 protocol/proto/envelope.proto：MessageEnvelope + MessageType 枚举 + ErrorResponse",
  "定义 protocol/proto/common.proto：Vec3 / EntityId / PlayerId / SceneId 等公共标量包装（禁止各模块自己定义坐标结构）",
  "定义 protocol/proto/command.proto：CommandHeader + `oneof` 占位（MovePlayer / CastSkill 等先放最小集合，后续任务增量扩展）",
  "定义 protocol/proto/query.proto 与 event.proto：QueryHeader / EventHeader 同样用 oneof 预留",
  "定义 protocol/flatbuffers/movement.fbs：位置/朝向/速度，**字段顺序写入后禁止调整**（FlatBuffers 兼容性要求）",
  "定义 protocol/flatbuffers/aoi.fbs：EntityEnter / EntityLeave / PositionUpdate 批量帧",
  "定义 protocol/flatbuffers/combat.fbs：DamageEvent / HealEvent / SkillCast 的最小字段集",
  "在 protocol/CMakeLists.txt 中接入 protoc 与 flatc 代码生成，产物输出到 build/generated，**不入库**",
  "实现 codec/protobuf_codec.h/.cpp 与 codec/flatbuf_codec.h/.cpp，统一实现 ICodec",
  "实现 envelope_validator：必填字段校验、版本校验、payload 非空校验，失败返回明确 ErrorCode",
  "写测试：往返编解码一致性（Protobuf / FlatBuffers 各 1000 次随机数据）；版本不匹配返回 VERSION_CONFLICT；缺失必填字段返回 INVALID_ARGUMENT；FlatBuffers 零拷贝验证（decode 不做内存拷贝，用 buffer 指针断言）",
  "写 benchmark：1e6 次小消息编解码耗时与分配次数",
  "写 docs/VERSIONING.md：协议演进规则（只增字段、禁改类型、禁重排 FBS 字段）",
 ],
 unit="Envelope 必填字段齐全性校验；Protobuf 编解码往返；FlatBuffers 编解码往返与零拷贝断言；版本校验（相同/更高/更低三种情况）；oneof payload 正确识别；错误码映射正确",
 integ="模拟跨进程：进程 A（测试内两个 Codec 实例模拟）编码 → 字节流经文件落盘 → 进程 B 读取并解码，结果一致；Protobuf 与 FlatBuffers 两条链路各自跑通；经济类消息携带 transaction_id + idempotency_key 透传不丢",
 bench="bin/protocol_bench：`pb_encode_ns=` / `pb_decode_ns=` / `fbs_encode_ns=` / `fbs_decode_ns=` / `alloc_per_op=`（FlatBuffers 解码目标 0 拷贝）",
 fail="payload 空字节：返回 INVALID_ARGUMENT；截断的字节流：解码返回错误而非崩溃（fuzz 1000 次随机截断，用 libFuzzer 或简易随机变异）；version 高于支持版本：VERSION_CONFLICT；恶意超大 payload（>16MB）：被 MaxPayloadBytes 拦截",
 accept=[
  "MessageEnvelope 九项字段全部定义且 C++ 可 Encode/Decode（单测断言每个字段往返一致）",
  "版本不匹配时返回 `VERSION_CONFLICT`，**不静默降级**",
  "FlatBuffers 解码路径分配次数 = 0（零拷贝，benchmark 实测）",
  "1000 次随机截断 fuzz 无崩溃、无越界（ASan 下跑）",
  "生成代码不入库（`git status` 不出现 *.pb.h / *_generated.h）",
  "protocol/docs/VERSIONING.md 存在且写明演进规则",
  "Debug / Release 双构建通过，ctest -R Protocol 全绿",
 ],
 forbid=[
  "禁止任何模块定义第二套消息头（grep 全仓，只认 protocol 模块的 Envelope）",
  "禁止手改 protoc / flatc 生成的代码",
  "禁止调整已发布 .fbs 文件的字段顺序",
  "禁止在协议中传递明文凭据",
  "禁止 payload 无上限（必须设 MaxPayloadBytes，默认 1MB，战斗帧另设）",
  "禁止协议版本不匹配时静默降级或跳过校验",
 ],
 perf="Protobuf 小消息（<256B）编解码 < 2us；FlatBuffers 编解码 < 300ns；FlatBuffers 解码分配次数 = 0；单条消息内存占用 < 协议大小的 1.5 倍。",
 deliver=["protocol/proto/envelope.proto", "protocol/proto/common.proto", "protocol/proto/command.proto",
          "protocol/proto/query.proto", "protocol/proto/event.proto",
          "protocol/flatbuffers/movement.fbs", "protocol/flatbuffers/aoi.fbs", "protocol/flatbuffers/combat.fbs",
          "protocol/include/mmo/protocol/codec/*.h", "protocol/src/codec/*.cpp",
          "protocol/tests/*", "protocol/docs/VERSIONING.md", "protocol/docs/INTERFACE.md", "protocol/CMakeLists.txt"],
 ctest="Protocol", both_build=True,
 bench_bins=[("bin/protocol_bench", "--iterations 1000000")],
 metrics=[("bench/protocol.txt", "fbs_decode_ns", "le", "300"),
          ("bench/protocol.txt", "fbs_decode_allocs", "le", "0")],
 artifacts=["protocol/proto/envelope.proto", "protocol/flatbuffers/movement.fbs", "protocol/docs/VERSIONING.md"],
),
# ------------------------------------------------------------------ 006
dict(
 id="TASK-006", name="RPC Framework（gRPC 统一封装）", phase="Phase 1 · 统一通信",
 objective="封装统一 gRPC 客户端/服务端：超时、取消、重试、错误映射（gRPC status ↔ mmo::core::Error）。**RPC 只允许用于跨进程、低频、管理面与数据面；禁止在战斗 Tick 内使用同步 RPC。**",
 deps="TASK-001, TASK-005",
 module="engine/rpc",
 owner=OWNER,
 inp="TASK-001 Error/Result；TASK-005 Protobuf 定义（服务定义基于 .proto）",
 out="engine/rpc 模块（GrpcClient / GrpcServer / 拦截器 / 重试策略）+ 端到端测试 + 超时与取消测试",
 iface="""```cpp
namespace mmo::rpc {
struct RpcOptions {                      // 每次调用必填，禁止用默认值蒙混
  DurationMs timeout_ms{500};
  bool       retry_on_unavailable{true};
  uint32_t   max_retries{2};
  DurationMs backoff_base_ms{20};        // 指数退避，带抖动
  bool       idempotent{false};          // 非幂等调用禁止自动重试
};
class GrpcChannelPool { public:          // 连接复用，禁止每次调用建连
  static Result<std::shared_ptr<GrpcChannelPool>> Create(size_t per_target = 4);
  Result<std::shared_ptr<grpc::Channel>> Get(std::string_view target);
};
template <typename Stub> class GrpcClient {
 public:
  GrpcClient(std::shared_ptr<GrpcChannelPool>, StubFactory);
  template <typename Req, typename Resp>
  Result<Resp> Call(Resp (Stub::*method)(grpc::ClientContext*, const Req&, Resp*),
                    const Req&, RpcOptions);
};
class GrpcServer { public:
  struct Config { std::string listen_addr; uint32_t max_threads{4};
                  DurationMs max_recv_msg_size{4*1024*1024}; };
  static Result<std::unique_ptr<GrpcServer>> Create(Config, std::vector<grpc::Service*>);
  Result<void> Start(); void Shutdown(DurationMs grace{5000});
};
core::Error MapStatus(const grpc::Status&) noexcept;   // 唯一映射入口
}
```""",
 data="""**gRPC status → mmo::core::Error 映射表（唯一真相）**

| gRPC code | mmo ErrorCode | retryable |
|---|---|---|
| OK | OK | - |
| INVALID_ARGUMENT | INVALID_ARGUMENT | 否 |
| NOT_FOUND | NOT_FOUND | 否 |
| DEADLINE_EXCEEDED | TIMEOUT | 是 |
| UNAVAILABLE | BUSY | 是（仅幂等） |
| RESOURCE_EXHAUSTED | RATE_LIMITED | 是（仅幂等） |
| UNAUTHENTICATED | UNAUTHORIZED | 否 |
| FAILED_PRECONDITION | VERSION_CONFLICT | 否 |
| INTERNAL / UNKNOWN | INTERNAL_ERROR | 否 |""",
 thread="gRPC 使用独立 completion queue 线程池（归属 Worker/Persistence 语义），**不得在 SimulationThread 发起同步调用**。客户端调用为异步 future + 业务线程 await（或投递回调），禁止阻塞 Tick 线程。",
 hot="NO", io="YES", rpc="YES", persist="NO",
 files=["engine/rpc/include/mmo/rpc/", "engine/rpc/src/", "engine/rpc/tests/", "engine/rpc/docs/", "protocol/proto/service/"],
 steps=[
  "定义 protocol/proto/service/*.proto：先只写 `HealthService` 与 `EchoService`（作为框架验证用），后续 DataService/ControlService 接口由 TASK-026 等增量扩展",
  "实现 grpc_channel_pool.h/.cpp：按 target 复用 channel，含空闲健康检查与重连",
  "实现 grpc_client.h：模板 Call 封装，强制传 RpcOptions，内部完成 deadline 设置、context 取消、重试与退避",
  "实现重试策略：仅当 `idempotent=true` 且 code ∈ {UNAVAILABLE, RESOURCE_EXHAUSTED, DEADLINE_EXCEEDED} 才重试；指数退避 + ±20% 抖动；每次重试必须带**同一个** idempotency_key",
  "实现 status_mapping.h/.cpp：上表映射，禁止其他地方写第二份 switch",
  "实现 grpc_server.h/.cpp：配置化启动、优雅关闭（grace period 内等待在途请求）、最大消息大小限制",
  "实现三个拦截器：TraceID 透传（从 metadata 取/放 trace_id）、日志拦截器（记录 method/latency/status）、限流拦截器（返回 RESOURCE_EXHAUSTED）",
  "实现客户端指标：per-method 的 QPS / 延迟分布 / 错误率 / 重试次数（为 TASK-039 指标采集预留接口）",
  "写测试：正常调用；超时（服务端 sleep 1s、客户端 timeout 200ms → TIMEOUT）；取消（客户端主动 cancel → CANCELLED 映射）；重试（服务端前两次返回 UNAVAILABLE → 第三次成功）；非幂等调用**不允许**重试（断言只调用一次）；错误码映射全覆盖表驱动测试",
  "写 docs/INTERFACE.md 与 docs/PERFORMANCE.md，首页写明「战斗 Tick 禁止同步 RPC」红线",
 ],
 unit="错误码映射表驱动全覆盖；RpcOptions 校验（timeout=0 → INVALID_ARGUMENT）；退避时间计算；非幂等不重试；channel 池复用（同 target 两次 Get 返回同一对象）",
 integ="起一个真实 GrpcServer（本机随机端口），客户端跑：Echo 成功、超时、取消、重试成功、重试耗尽、限流六种场景；TraceID 拦截器验证服务端日志能拿到客户端传入的 trace_id；优雅关闭验证在途请求被等待完成而非硬杀",
 bench="bin/rpc_bench：本机回环 1e5 次 Echo，`rpc_p50_us=` / `rpc_p99_us=` / `rpc_qps=`；超时场景 `timeout_overhead_us=`",
 fail="服务端未启动：客户端返回 UNAVAILABLE→BUSY 且不崩溃；服务端进程被 kill：客户端在 timeout 内返回，不挂死；网络分区（用 iptables/断网模拟或指向不可达地址）：按退避重试后失败并返回明确错误；消息体超限：服务端拒绝并返回 RESOURCE_EXHAUSTED；重试风暴保护：连续失败时退避上限 1s，禁止无限快速重试",
 accept=[
  "gRPC status → Error 映射表 100% 覆盖（表驱动单测）且全仓唯一实现",
  "超时、取消、重试成功、重试耗尽四种路径均有集成测试且通过",
  "**非幂等调用重试次数 = 0**（单测断言服务端只收到 1 次）",
  "TraceID 跨进程透传验证通过（服务端日志含客户端 trace_id）",
  "优雅关闭：在途请求完成，grace 超时后强制关闭且记录日志",
  "grep 验证：engine/rpc 之外无 `grpc::` 直接调用（统一走封装）",
  "benchmark 输出 p50/p99/QPS 并写入 docs/PERFORMANCE.md",
  "Debug / Release 双构建通过，ctest -R Rpc 全绿",
 ],
 forbid=[
  "禁止在 Combat / Movement / AOI / Buff Tick 内发起同步 RPC（红线扫描 + 代码评审）",
  "禁止绕过 GrpcClient 直接使用 grpc::Stub",
  "禁止对非幂等调用启用自动重试",
  "禁止无限重试或固定间隔重试（必须指数退避 + 抖动 + 上限）",
  "禁止每次调用新建 channel",
  "禁止在 RPC 路径上吞掉错误返回 bool",
 ],
 perf="本机回环 Echo：p50 < 200us、p99 < 1ms；单连接 QPS > 20k；超时控制误差 < 10ms；客户端单次调用额外开销（相对裸 stub）< 5us。",
 deliver=["engine/rpc/include/mmo/rpc/grpc_client.h", "engine/rpc/include/mmo/rpc/grpc_server.h",
          "engine/rpc/include/mmo/rpc/status_mapping.h", "engine/rpc/include/mmo/rpc/grpc_channel_pool.h",
          "engine/rpc/src/*.cpp", "engine/rpc/tests/*", "protocol/proto/service/*.proto",
          "engine/rpc/docs/INTERFACE.md", "engine/rpc/docs/PERFORMANCE.md"],
 ctest="Rpc", both_build=True,
 bench_bins=[("bin/rpc_bench", "--iterations 100000")],
 metrics=[("bench/rpc.txt", "rpc_p99_us", "le", "1000")],
 artifacts=["engine/rpc/include/mmo/rpc/status_mapping.h", "engine/rpc/docs/INTERFACE.md"],
 ports=[],
),
# ------------------------------------------------------------------ 007
dict(
 id="TASK-007", name="Command / Query / Event Bus", phase="Phase 1 · 统一通信",
 objective="实现进程内三条总线：CommandBus（执行操作）、QueryBus（只读查询，无副作用）、EventBus（已发生的事实，异步处理）。提供 Register / Dispatch / Subscribe / Unsubscribe，并跑通 Command→Handler→Event→Subscriber 的 Demo 链路。",
 deps="TASK-001, TASK-004, TASK-005",
 module="engine/core",
 owner=OWNER,
 inp="TASK-001 Result；TASK-004 Scheduler（EventBus 异步派发）；TASK-005 Envelope（命令携带 trace/request id）",
 out="engine/core 的 bus 子模块 + Demo 链路 + 背压与异步派发测试",
 iface="""```cpp
namespace mmo::core {
// ---- CommandBus：1 command : 1 handler，同步执行，返回结果 ----
template <typename TCommand> class ICommandHandler {
 public: virtual ~ICommandHandler() = default;
  virtual Result<typename TCommand::Result> Handle(const TCommand&, const CommandContext&) = 0; };
class CommandBus {
 public:
  template <typename TCommand, typename H> Result<void> Register(std::shared_ptr<H>);
  template <typename TCommand> Result<typename TCommand::Result> Dispatch(const TCommand&, CommandContext);
  template <typename TCommand, typename Fn> Result<void> RegisterFn(Fn&&);   // 轻量注册
  size_t RegisteredCount() const noexcept; size_t InFlight() const noexcept; };
// ---- QueryBus：只读，编译期+运行期禁止副作用 ----
class QueryBus { public:
  template <typename TQuery, typename Fn> Result<void> RegisterFn(Fn&&);
  template <typename TQuery> Result<typename TQuery::Result> Ask(const TQuery&, QueryContext); };
// ---- EventBus：1 event : N subscriber，异步 ----
class EventBus { public:
  using SubId = uint64_t;
  template <typename TEvent> Result<SubId> Subscribe(std::function<void(const TEvent&)>);
  Result<void> Unsubscribe(SubId);
  template <typename TEvent> Result<void> Publish(const TEvent&);           // 入队，不立即执行
  template <typename TEvent> Result<void> PublishImmediate(const TEvent&);  // 同线程立即派发
  Result<size_t> Drain(size_t max_events, DurationMs budget);               // 由宿主线程驱动
  size_t QueueDepth() const noexcept; size_t DroppedCount() const noexcept; };
struct CommandContext { TraceID trace_id; RequestID request_id; PlayerID player_id; SceneID scene_id; };
}
```""",
 data="""| 概念 | 语义 | 返回 | 副作用 |
|---|---|---|---|
| Command | 「请执行一个操作」 | Result&lt;T&gt; | 允许，且必须可审计 |
| Query | 「读取数据」 | Result&lt;T&gt; | **禁止** |
| Event | 「已经发生的事实」 | void | 由订阅者决定，发布者不关心 |

**Command 必带字段**：RequestID / PlayerID / Source / Timestamp / Version；经济类额外带 TransactionID / IdempotencyKey。
**Event 默认异步**，由宿主线程 Drain 派发，禁止在 Tick 中间无限派发。""",
 thread="CommandBus / QueryBus 在**调用者线程**同步执行（通常是 SimulationThread）。EventBus 入队无锁（MPMC，来自 TASK-004），Drain 由 SimulationThread 在 Tick 的 Event 阶段调用并**带时间预算**，超时留到下帧。禁止 EventBus 自带线程。",
 hot="YES（Command/Event 位于 Tick 热路径）", io="NO", rpc="NO", persist="NO",
 files=["engine/core/include/mmo/core/bus/", "engine/core/src/bus/", "engine/core/tests/", "engine/core/benchmark/", "engine/core/docs/"],
 steps=[
  "实现 bus/command.h：Command 概念约束（必须含 RequestID 等字段，用 concept 静态断言）",
  "实现 bus/command_bus.h/.cpp：类型安全的 Handler 注册（std::type_index → 类型擦除的 handler 槽），Dispatch 返回结果，未注册返回 NOT_FOUND",
  "实现 bus/query_bus.h/.cpp：同构实现，Ask 只读；加编译期标注与运行期文档约束「禁止副作用」",
  "实现 bus/event_bus.h/.cpp：MPMC 队列 + 类型擦除订阅表；Publish 入队；Drain(now, max_events, budget) 批量派发",
  "实现背压策略：队列满时按配置丢弃**非关键**事件并计数（DroppedCount 指标），关键事件（经济类）禁止丢弃，改为返回 BUSY 让调用方处理",
  "实现 Unsubscribe 幂等；订阅者抛错时捕获记录并继续派发给其他订阅者（禁止一个坏订阅者拖垮总线）",
  "实现 Drain 时间预算：单个 Tick 中 Event 阶段默认 2ms 预算，超时立即返回剩余数量",
  "写 Demo（tests/demo_pipeline.cpp）：Dispatch(MovePlayerCommand) → Handler 处理 → Publish(PlayerMovedEvent) → 两个 Subscriber 收到 → 断言顺序与内容",
  "写测试：未注册 Command 返回 NOT_FOUND；Query 无副作用（在 handler 中试图修改状态 → 通过测试替身检测到并失败）；Event 多订阅者按注册顺序派发；Unsubscribe 后不再收到；Drain 预算生效",
  "写 benchmark：1e6 次 Command Dispatch、1e6 次 Event Publish+Drain",
  "写 docs/README.md（Command/Query/Event 选型决策树）与 docs/INTERFACE.md",
 ],
 unit="Command 注册/重复注册（返回错误）/未注册 Dispatch；Query Ask 与错误传播；Event 订阅/退订/多播；Unsubscribe 幂等；Drain 预算与剩余计数；订阅者异常隔离；背压丢弃计数",
 integ="Demo 全链路：Command → Handler → Event → 2 个 Subscriber，断言执行顺序与 payload 一致；跨线程（Worker 投递 Command 到 Simulation）验证上下文（trace_id）不丢失；1e5 次混合负载下无死锁、无丢事件（关键事件零丢弃）",
 bench="bin/bus_bench：`cmd_dispatch_ns=` / `event_publish_ns=` / `event_drain_ns_per_event=` / `alloc_per_cmd=`",
 fail="Handler 抛异常：捕获转成 INTERNAL_ERROR，总线状态不变可继续服务；Subscriber 抛异常：记录日志 + 指标，其余订阅者照常收到；队列满：非关键事件丢弃并计数，关键事件返回 BUSY；Drain 预算耗尽：返回剩余数量且不阻塞；重复注册同一 Command：返回错误而非覆盖（防静默覆盖——这是 CAMI 踩过的坑）",
 accept=[
  "Demo 链路跑通：Dispatch(Command) → Handler → Publish(Event) → Subscriber 收到（集成测试断言）",
  "Query 无副作用：测试替身可检测到任何写操作并判定失败",
  "EventBus **不创建线程**（grep 验证），Drain 由宿主驱动且带时间预算",
  "队列满时：非关键事件丢弃并计数，关键事件返回 BUSY，行为可被单测断言",
  "一个订阅者抛异常不影响其他订阅者（单测覆盖）",
  "重复注册同一 Command 类型返回错误，不静默覆盖",
  "benchmark 指标达标并写入 docs/PERFORMANCE.md",
  "Debug / Release 双构建通过，ctest -R Core_Bus 全绿",
 ],
 forbid=[
  "禁止 EventBus 自带线程（必须宿主 Drain）",
  "禁止在 Query handler 中修改任何状态",
  "禁止在 Tick 中间无限派发事件（必须带预算）",
  "禁止让一个异常的订阅者中断整个事件派发",
  "禁止静默覆盖已注册的 Handler（必须返回错误）",
  "禁止把进程内模块调用改造成网络 RPC",
  "禁止丢关键（经济类）事件",
 ],
 perf="Command Dispatch < 150ns；Event Publish < 100ns；Event Drain < 80ns/event；单次 Command 处理堆分配 = 0（走 Arena/Pool）；1e6 事件队列内存占用 < 64MB。",
 deliver=["engine/core/include/mmo/core/bus/command_bus.h", "engine/core/include/mmo/core/bus/query_bus.h",
          "engine/core/include/mmo/core/bus/event_bus.h", "engine/core/src/bus/*.cpp",
          "engine/core/tests/demo_pipeline.cpp", "engine/core/tests/*", "engine/core/benchmark/*",
          "engine/core/docs/README.md", "engine/core/docs/INTERFACE.md", "engine/core/docs/PERFORMANCE.md"],
 ctest="Core_Bus", both_build=True,
 bench_bins=[("bin/bus_bench", "--iterations 1000000")],
 metrics=[("bench/core_bus.txt", "cmd_dispatch_ns", "le", "150"),
          ("bench/core_bus.txt", "event_drain_ns_per_event", "le", "80")],
 artifacts=["engine/core/include/mmo/core/bus/command_bus.h", "engine/core/tests/demo_pipeline.cpp"],
 scan=[("engine/core/src/bus", r"std::thread")],
),
# ------------------------------------------------------------------ 008
dict(
 id="TASK-008", name="Network Transport（TCP 第一版）", phase="Phase 2 · Gateway",
 objective="实现 INetworkTransport 抽象与 TCP 第一版实现：Connection / Packet / Buffer / Encode / Decode / Send / Receive，并通过 1K / 5K / 10K 连接基准测试。未来可扩展 UDP / QUIC 而不改动上层。",
 deps="TASK-004, TASK-005",
 module="engine/net",
 owner=OWNER,
 inp="TASK-004 MPMC 队列与线程；TASK-005 协议编解码",
 out="engine/net 模块（transport 抽象 + TCP 实现 + 缓冲管理）+ 连接数基准 + 长稳测试",
 iface="""```cpp
namespace mmo::net {
class IConnection { public: virtual ~IConnection() = default;
  virtual ConnectionId Id() const noexcept = 0;
  virtual Result<void> Send(std::span<const uint8_t>) = 0;      // 零拷贝语义：写入发送缓冲
  virtual Result<void> Close(CloseReason) noexcept = 0;
  virtual std::string_view RemoteAddr() const noexcept = 0;
  virtual ConnectionStats Stats() const noexcept = 0; };
struct TransportEvent {                                          // 事件驱动，禁止回调重入
  enum class Kind { Connected, Disconnected, Received, SendDrained, Error } kind;
  ConnectionId conn_id; core::Error error; std::span<const uint8_t> data; };
class INetworkTransport { public: virtual ~INetworkTransport() = default;
  virtual Result<void> Listen(std::string_view addr, uint16_t port) = 0;
  virtual Result<void> Stop() noexcept = 0;
  virtual Result<void> Poll(DurationMs timeout, std::vector<TransportEvent>& out) = 0;  // 由宿主线程驱动
  virtual size_t ConnectionCount() const noexcept = 0;
  virtual TransportStats Stats() const noexcept = 0; };
std::unique_ptr<INetworkTransport> CreateTcpTransport(TcpConfig);   // 第一版
// TcpConfig: io_threads=2, max_connections=50000, recv_buf=64KB, send_buf=256KB,
//            tcp_nodelay=true, keepalive_idle=30s, backlog=1024
}
```""",
 data="""| 结构 | 说明 |
|---|---|
| ConnectionId | uint64，单调递增，复用 SlotMap 索引避免 ABA |
| Buffer | 环形缓冲，读写指针分离；发送缓冲满时返回 BUSY 而非阻塞 |
| Packet | 4 字节长度前缀 + payload（大端），长度上限由 MaxPayloadBytes 约束 |
| TransportStats | conn_count / bytes_in / bytes_out / packets_in / packets_out / send_queue_depth / error_count |""",
 thread="IO 线程（NetworkThread）只做 Poll + 数据搬运，**不做业务解包**。解包与处理投递到 Worker/Simulation 线程。每个连接的状态由所属 IO 线程独占，跨线程操作通过事件队列。",
 hot="YES（收发位于网络热路径）", io="YES", rpc="NO", persist="NO",
 files=["engine/net/include/mmo/net/", "engine/net/src/", "engine/net/tests/", "engine/net/benchmark/", "engine/net/docs/"],
 steps=[
  "实现 buffer.h：环形缓冲（读/写双指针，支持 scatter/gather iovec）",
  "实现 connection.h/.cpp：ConnectionId 分配（SlotMap）、发送队列、流量统计、优雅关闭（先发完发送缓冲再 FIN）",
  "实现 tcp_transport.h/.cpp：Windows 用 IOCP / Linux 用 epoll（先做跨平台抽象层 `poller.h`，第一版 Windows 优先，因为本地验证在 Windows）",
  "实现 Poll 事件模型：单次 Poll 收集批量事件，禁止回调重入业务代码",
  "实现粘包/半包处理：长度前缀解析，半包保留在连接缓冲，禁止丢弃或假设一帧一包",
  "实现背压：单连接发送缓冲上限 256KB，超限返回 BUSY 并由上层决定（限速/断开），禁止无限堆积",
  "实现连接数限制与拒绝策略：超过 max_connections 直接拒绝新连接并计数",
  "实现 keepalive 与空闲超时清理（心跳由 TASK-009 负责，本任务只做传输层 keepalive）",
  "写测试：单连接 echo 往返；半包/粘包（一次发 0.5 包、一次发 3 包）；大包（1MB）分片收发；连接关闭后资源回收；错误地址/端口占用返回明确 Error",
  "写 benchmark 工具 tools/netbench/：起 N 个客户端连接，测量 `conn_established_ms` / `pps` / `mbps` / `cpu_percent` / `mem_mb` / `fd_count`",
  "跑 1K / 5K / 10K 连接基准并把结果写入 docs/PERFORMANCE.md",
 ],
 unit="环形缓冲读写/回绕/满/空；ConnectionId 唯一与复用安全；长度前缀解析（半包、粘包、超大包拒绝）；发送缓冲背压与 BUSY；SlotMap 回收后不误发旧连接",
 integ="本机起服务端 + 1000 个客户端连接，每个客户端每秒发 20 条 64B 消息，跑 60 秒：无断连、无消息错乱、服务端收到的消息总数 = 期望值；中途 kill 100 个客户端，服务端正确感知断连并回收资源（连接数回落）",
 bench="bin/net_bench + tools/netbench：1K / 5K / 10K 连接各跑 60 秒，输出 `conn_count` / `established_ms` / `pps` / `mbps` / `cpu_percent` / `rss_mb` / `per_conn_mem_kb`",
 fail="端口被占用：Listen 返回明确错误而非崩溃；客户端异常断连（RST）：服务端收到 Disconnected 事件并回收；发送缓冲持续打满：返回 BUSY 并记录指标，不 OOM；半开连接（只连不发）：keepalive + 空闲超时清理生效；突发 10000 连接同时建立：不崩溃、建立耗时可测、失败连接被正确计数",
 accept=[
  "1K / 5K / 10K 连接基准全部跑完，结果数据写入 docs/PERFORMANCE.md（含 CPU / 内存 / PPS）",
  "半包与粘包处理正确（单测 + 集成测试双重覆盖）",
  "单连接发送缓冲背压生效，不出现无界内存增长（长稳 10 分钟 RSS 平稳）",
  "异常断连可被感知并回收资源，连接计数准确回落",
  "传输层**不含任何游戏逻辑**（grep：net/ 目录无 combat/scene/quest 字样）",
  "上层只依赖 INetworkTransport 抽象（grep：除 factory 外无 TcpTransport 直接引用）",
  "Debug / Release 双构建通过，ctest -R Net 全绿",
 ],
 forbid=[
  "禁止在 IO 线程做业务解包或数据库访问",
  "禁止回调重入业务代码（必须事件队列）",
  "禁止发送缓冲无界增长",
  "禁止假设「一次 recv = 一个完整包」",
  "禁止在传输层引入游戏语义（玩家、场景、战斗）",
  "禁止上层直接依赖 TCP 实现（只依赖抽象）",
 ],
 perf="10K 空闲连接：RSS < 200MB、per-conn 内存 < 20KB、CPU 空闲 < 2%；1K 活跃连接 20 msg/s：p99 投递延迟 < 1ms；单核 PPS > 100k（64B 消息）；连接建立 10K 总耗时 < 3s。",
 deliver=["engine/net/include/mmo/net/transport.h", "engine/net/include/mmo/net/connection.h",
          "engine/net/include/mmo/net/buffer.h", "engine/net/src/*.cpp", "engine/net/tests/*",
          "engine/net/benchmark/*", "tools/netbench/*", "engine/net/docs/INTERFACE.md",
          "engine/net/docs/PERFORMANCE.md"],
 ctest="Net", both_build=True,
 # §22 的 per-conn ≤20KB 针对「空闲连接」（recv_buf 延迟分配 0 字节），
 # 验收必须跑 --idle；活跃基准（pps/mbps）由人工跑并写入 PERFORMANCE.md。
 bench_bins=[("bin/net_bench", "--connections 10000 --duration 60 --idle")],
 metrics=[("bench/net_10k_idle.txt", "per_conn_mem_kb", "le", "20")],
 artifacts=["engine/net/include/mmo/net/transport.h", "engine/net/docs/PERFORMANCE.md"],
),
# ------------------------------------------------------------------ 009
dict(
 id="TASK-009", name="Session 管理", phase="Phase 2 · Gateway",
 objective="实现 Session 与 SessionManager：会话生命周期、鉴权、心跳、断线处理与重连准备。Session 字段固定为 SessionID / PlayerID / GatewayID / GameNodeID / SceneID / Version。",
 deps="TASK-008",
 module="server/gateway",
 owner=OWNER,
 inp="TASK-008 传输层事件；PROJECT_REQUIREMENTS.md 第 33 节 Session 结构",
 out="server/gateway 的 session 子模块 + 心跳/超时/重连测试",
 iface="""```cpp
namespace mmo::gateway {
enum class SessionState : uint8_t { Connecting, Authenticating, Active, Suspended, Closing, Closed };
struct Session {
  SessionId    session_id;    PlayerId  player_id;
  GatewayId    gateway_id;    NodeId    game_node_id;
  SceneId      scene_id;      uint32_t  version{0};
  core::SteadyTime last_heartbeat;
  SessionState state;         net::ConnectionId conn_id;
  core::TraceID trace_id;
};
class ISessionStore { public: virtual ~ISessionStore() = default;   // 为 TASK-027 Redis 预留
  virtual core::Result<void> Put(const Session&) = 0;
  virtual core::Result<std::optional<Session>> Get(SessionId) = 0;
  virtual core::Result<std::optional<Session>> FindByPlayer(PlayerId) = 0;
  virtual core::Result<void> Remove(SessionId) = 0; };
class SessionManager final {
 public:
  struct Config { DurationMs heartbeat_interval{5000}; uint32_t max_missed{3};
                  DurationMs suspend_grace{30000}; size_t max_sessions{50000}; };
  core::Result<SessionId> OnConnected(net::ConnectionId);
  core::Result<void> OnAuthenticate(SessionId, PlayerId, AuthToken);
  core::Result<void> OnHeartbeat(SessionId);
  core::Result<void> OnDisconnected(SessionId, net::CloseReason);
  core::Result<void> Reattach(SessionId, net::ConnectionId, uint32_t expected_version); // 重连核心
  core::Result<void> Tick(core::SteadyTime now);   // 扫描超时，由宿主驱动
  size_t ActiveCount() const noexcept; size_t SuspendedCount() const noexcept; };
}
```""",
 data="""**Session 状态机**

```
Connecting ──auth ok──> Authenticating ──> Active
     │                       │                │
     │                   auth fail         disconnect
     ▼                       ▼                ▼
   Closed                 Closed          Suspended ──grace 内重连──> Active
                                               │
                                          grace 超时
                                               ▼
                                             Closed
```

- `version` 每次重连 +1，用于防止旧连接回放。
- Suspended 会话在 grace 期内保留，超时即释放（禁止无限堆积）。""",
 thread="SessionManager 由 Gateway 的 NetworkThread 驱动 Tick；Session 状态由单线程拥有，跨线程查询通过不可变快照或消息队列。禁止用全局锁保护 Session 表。",
 hot="YES（心跳处理位于热路径）", io="NO", rpc="NO", persist="NO（持久化由 TASK-027 Redis 适配器实现）",
 files=["server/gateway/include/mmo/gateway/session/", "server/gateway/src/session/", "server/gateway/tests/", "server/gateway/docs/"],
 steps=[
  "实现 session/session.h：Session 结构与 SessionId/SessionState 定义，字段严格按规范七项 + 内部字段",
  "实现 session/session_store.h：ISessionStore 接口 + InMemorySessionStore 第一版实现（为 Redis 预留同接口）",
  "实现 session/session_manager.h/.cpp：六状态机、OnConnected/OnAuthenticate/OnHeartbeat/OnDisconnected/Reattach/Tick",
  "实现心跳检测：Tick 扫描超时会话（interval × max_missed），超时转 Suspended 并发事件",
  "实现 Reattach：校验 expected_version 匹配才允许接管，防止旧连接回放；成功后 version+1 并广播 SessionReattached 事件",
  "实现容量上限：max_sessions 达到后拒绝新连接并返回 BUSY（禁止无界增长）",
  "实现鉴权接口抽象 IAuthProvider（第一版用本地 token 校验替身，禁止写死口令）",
  "实现会话事件：SessionCreated / SessionAuthenticated / SessionSuspended / SessionResumed / SessionClosed，全部走 EventBus",
  "写测试：状态机全路径（含非法转移返回错误）；心跳超时转 Suspended；grace 内 Reattach 成功；grace 超时释放；version 不匹配的 Reattach 被拒绝；容量上限行为",
  "写集成测试：模拟 1000 会话心跳压测 + 100 会话同时断线重连",
  "写 docs/INTERFACE.md 与 docs/README.md（含状态机图）",
 ],
 unit="六状态机合法/非法转移全覆盖；心跳计时准确；version 递增与校验；容量上限；SessionStore CRUD；事件发布正确",
 integ="1000 个会话并发心跳，Tick 扫描耗时可测且 < 1ms；模拟 100 个会话同时断线后 5 秒内全部重连成功且 version 正确递增；断线期间发往该会话的消息被丢弃或缓存（按配置）且不下发给旧连接",
 bench="bin/session_bench：`session_heartbeat_ns=` / `session_tick_us_10k=` / `reattach_ns=` / `per_session_bytes=`",
 fail="心跳风暴（1 万会话同一秒发心跳）：不丢、不崩、Tick 耗时可测；鉴权失败：转 Closed 并记录，不保留悬挂会话；Reattach 时旧连接仍在线：旧连接被强制关闭且事件顺序正确；SessionStore 不可用（注入故障）：返回明确错误，不吞异常；grace 期内的会话不会被误释放（边界时间测试）",
 accept=[
  "Session 七项字段（SessionID/PlayerID/GatewayID/GameNodeID/SceneID/Version + 心跳）全部实现",
  "六状态机全部路径有单测，非法转移返回 INVALID_ARGUMENT",
  "version 不匹配的 Reattach 被拒绝（防回放，单测断言）",
  "grace 期内可重连、超期释放，两个边界都有测试",
  "1000 会话心跳 Tick 扫描 < 1ms（benchmark 实测）",
  "会话容量上限生效，不无界增长",
  "SessionManager 不使用全局锁（代码评审确认）",
  "Debug / Release 双构建通过，ctest -R Gateway_Session 全绿",
 ],
 forbid=[
  "禁止用全局锁保护 Session 表",
  "禁止无限保留 Suspended 会话（必须 grace 超时释放）",
  "禁止 Reattach 时不校验 version",
  "禁止在 Session 中保存玩家最终持久化数据（那是 DataService 职责）",
  "禁止把鉴权口令写死在代码里",
  "禁止在心跳处理中做阻塞 IO",
 ],
 perf="10K 会话心跳 Tick 扫描 < 1ms；单会话内存占用 < 256B；Reattach 处理 < 10us；心跳处理 < 200ns/次。",
 deliver=["server/gateway/include/mmo/gateway/session/session.h",
          "server/gateway/include/mmo/gateway/session/session_manager.h",
          "server/gateway/include/mmo/gateway/session/session_store.h",
          "server/gateway/src/session/*.cpp", "server/gateway/tests/*",
          "server/gateway/docs/INTERFACE.md", "server/gateway/docs/README.md"],
 ctest="Gateway_Session", both_build=True,
 bench_bins=[("bin/session_bench", "--sessions 10000")],
 metrics=[("bench/gateway_session.txt", "session_tick_us_10k", "le", "1000"),
          ("bench/gateway_session.txt", "per_session_bytes", "le", "256")],
 artifacts=["server/gateway/include/mmo/gateway/session/session_manager.h"],
),
]
