// engine/core/tests/log_test.cpp — TASK-002 单元测试 / 集成测试 / 失败测试
//
// 自包含 harness（不依赖 gtest，vcpkg 离线不可用亦可验证），与 error_test.cpp 同风格：
// 纯 CHECK 断言 + 全局 operator new 计数（用于「单条日志零堆分配」断言）。
//
// 覆盖：
//   §16 单测   LogLevel 解析 / TraceID 唯一性与单调性 / ScopedLogContext 嵌套恢复 /
//             环形队列 SPSC 顺序与满队列语义 / 时间戳格式化 / 格式串编译期校验（见 TEST.md）
//   §17 集成   跨线程 WithContext 携带 TraceID -> 落盘 -> 反查同一 Trace 的全部日志 /
//             RotatingFileSink 滚动后的文件数量与命名 / JSON 模式九字段齐全
//   §19 失败   队列打满不阻塞不崩溃且丢弃计数正确并产生一条 Warn /
//             后台线程被卡住时业务线程仍可写入 / 打开失败返回 Error / Fatal 立即落盘
//   §20 验收   关闭日志开销 / 单条日志零堆分配

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <new>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "mmo/core/log/log_sink.h"
#include "mmo/core/log/logger.h"
#include "mmo/core/log/trace_id.h"

// 红线合规输出通道：禁止 std::cout / printf / std::cerr，统一走 fwrite。
#include "test_print.h"

// 白盒测试：环形队列与行格式化器是模块内部实现，不进 PUBLIC 接口。
#include "log/async_ring_buffer.h"
#include "log/log_formatter.h"

// ---- 全局分配计数器 ----
namespace {
std::size_t g_alloc_count = 0;
}  // namespace

void* operator new(std::size_t n) {
    g_alloc_count++;
    void* p = std::malloc(n);
    if (!p) std::abort();
    return p;
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }

namespace {

int failures = 0;

#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            ::mmo::core::test::ErrorFmt("FAIL @ %s:%d : %s\n", __FILE__,        \
                                        __LINE__, #cond);                      \
            failures++;                                                        \
        }                                                                      \
    } while (0)

using namespace mmo::core;

// ---- 测试用 sink ---------------------------------------------------------
class CaptureSink final : public ILogSink {
public:
    // store_all=false 时只统计条数与挑出 Warn，避免压测场景占用大量内存。
    explicit CaptureSink(bool store_all = true, int block_once_ms = 0)
        : store_all_(store_all), block_once_ms_(block_once_ms) {}

    void Write(const LogRecord& rec) noexcept override {
        if (block_once_ms_ > 0 && !blocked_) {
            blocked_ = true;
            std::this_thread::sleep_for(std::chrono::milliseconds(block_once_ms_));
        }
        total_.fetch_add(1, std::memory_order_relaxed);
        if (rec.level == LogLevel::Warn) {
            warn_total_.fetch_add(1, std::memory_order_relaxed);
        }
        if (store_all_) {
            records.push_back(rec);
        }
    }

    std::uint64_t Total() const noexcept { return total_.load(std::memory_order_relaxed); }
    std::uint64_t WarnTotal() const noexcept { return warn_total_.load(std::memory_order_relaxed); }

    std::vector<LogRecord> records;

private:
    bool store_all_;
    int block_once_ms_;
    bool blocked_ = false;
    std::atomic<std::uint64_t> total_{0};
    std::atomic<std::uint64_t> warn_total_{0};
};

std::string ReadFile(const std::filesystem::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

std::size_t CountLinesContaining(const std::string& text, const std::string& needle) {
    std::size_t count = 0;
    std::size_t pos = 0;
    for (;;) {
        const std::size_t hit = text.find(needle, pos);
        if (hit == std::string::npos) {
            break;
        }
        ++count;
        const std::size_t eol = text.find('\n', hit);
        if (eol == std::string::npos) {
            break;
        }
        pos = eol + 1;
    }
    return count;
}

const std::filesystem::path& TestDir() {
    static const std::filesystem::path dir =
        std::filesystem::temp_directory_path() / "mmo_core_log_test";
    return dir;
}

void ResetTestDir() {
    std::filesystem::create_directories(TestDir());
    for (const auto& entry : std::filesystem::directory_iterator(TestDir())) {
        std::filesystem::remove_all(entry.path());
    }
}

// ===========================================================================
// §16 单元测试
// ===========================================================================
void TestLogLevel() {
    const LogLevel all[] = {LogLevel::Trace, LogLevel::Debug, LogLevel::Info,
                            LogLevel::Warn,  LogLevel::Error, LogLevel::Fatal};
    for (const LogLevel lv : all) {
        const auto parsed = ParseLogLevel(ToString(lv));
        CHECK(parsed.has_value());
        CHECK(parsed.value() == lv);
    }
    // 大小写不敏感 + 别名
    CHECK(ParseLogLevel("warn").value() == LogLevel::Warn);
    CHECK(ParseLogLevel("WARNING").value() == LogLevel::Warn);
    CHECK(ParseLogLevel("Err").value() == LogLevel::Error);
    CHECK(ParseLogLevel("critical").value() == LogLevel::Fatal);
    CHECK(!ParseLogLevel("nope").has_value());
    CHECK(!ParseLogLevel("").has_value());
    CHECK(!ParseLogLevel("INFO ").has_value());  // 补空格的显示名不是配置名
}

void TestTraceId() {
    // 节点号
    SetNodeId(7);
    const TraceID with_node = NewTraceID();
    CHECK(NodeOf(with_node) == 7);
    CHECK(NodeId() == 7);
    SetNodeId(1);

    // 100 万次：唯一（严格递增即唯一）+ 单调 + 节点号正确
    constexpr int kN = 1000000;
    std::vector<TraceID> ids;
    ids.reserve(kN);
    for (int i = 0; i < kN; ++i) {
        ids.push_back(NewTraceID());
    }
    bool strictly_increasing = true;
    for (int i = 1; i < kN; ++i) {
        if (ids[i] <= ids[i - 1]) {
            strictly_increasing = false;
            break;
        }
    }
    CHECK(strictly_increasing);
    CHECK(NodeOf(ids.front()) == 1);
    CHECK(NodeOf(ids.back()) == 1);

    // 同一 Trace 派生出的 RequestID 互不相同，且低 48 位与 Trace 一致（可反查所属 Trace）
    const TraceID trace = NewTraceID();
    const RequestID r1 = DeriveRequestID(trace);
    const RequestID r2 = DeriveRequestID(trace);
    CHECK(r1 != r2);
    CHECK(LowOf(r1) == LowOf(trace));
    CHECK(LowOf(r2) == LowOf(trace));
}

void TestLogContext() {
    // 初始为空
    CHECK(CurrentLogContext().trace_id == kInvalidTraceId);

    {
        LogContext patch{};
        patch.trace_id = 0xABCD;
        patch.module = "core.test";
        ScopedLogContext outer(patch);
        CHECK(CurrentLogContext().trace_id == 0xABCD);
        CHECK(CurrentLogContext().module == "core.test");

        {
            // 内层只覆盖 player：trace / module 必须继承外层
            LogContext inner_patch{};
            inner_patch.player_id = 42;
            ScopedLogContext inner(inner_patch);
            CHECK(CurrentLogContext().trace_id == 0xABCD);
            CHECK(CurrentLogContext().module == "core.test");
            CHECK(CurrentLogContext().player_id == 42);
        }
        // 内层析构后恢复外层
        CHECK(CurrentLogContext().trace_id == 0xABCD);
        CHECK(CurrentLogContext().player_id == kInvalidPlayerId);
    }
    // 外层析构后回到空
    CHECK(CurrentLogContext().trace_id == kInvalidTraceId);
    CHECK(CurrentLogContext().module.empty());

    // WithContext：整体替换（含把字段清空）
    LogContext full{};
    full.trace_id = 0x1234;
    full.player_id = 9;
    WithContext(full, [] {
        CHECK(CurrentLogContext().trace_id == 0x1234);
        CHECK(CurrentLogContext().player_id == 9);
    });
    CHECK(CurrentLogContext().trace_id == kInvalidTraceId);
}

void TestRingBuffer() {
    // SPSC 顺序：序号写进 message，出队后必须严格递增
    mmo::core::detail::AsyncRingBuffer ring(1024);
    const int total = 1000;
    int enqueued = 0;
    for (int i = 0; i < total; ++i) {
        LogRecord rec{};
        rec.message_len = static_cast<std::uint32_t>(
            static_cast<std::size_t>(std::snprintf(rec.message, sizeof(rec.message), "%d", i)));
        rec.packed_size = static_cast<std::uint32_t>(PackedSize(rec));
        if (ring.TryEnqueue(rec)) {
            ++enqueued;
        }
    }
    CHECK(enqueued == total);

    LogRecord out{};
    int expected = 0;
    while (ring.TryDequeue(out)) {
        CHECK(std::strcmp(out.message, std::to_string(expected).c_str()) == 0);
        ++expected;
    }
    CHECK(expected == total);

    // 满队列：容量 8，第 9 条必须失败（丢弃而非阻塞）
    mmo::core::detail::AsyncRingBuffer small(8);
    for (int i = 0; i < 8; ++i) {
        LogRecord rec{};
        rec.packed_size = static_cast<std::uint32_t>(PackedSize(rec));
        CHECK(small.TryEnqueue(rec));
    }
    LogRecord extra{};
    extra.packed_size = static_cast<std::uint32_t>(PackedSize(extra));
    CHECK(!small.TryEnqueue(extra));
    // 出队一条后应可再次入队（槽位被正确归还）
    CHECK(small.TryDequeue(out));
    CHECK(small.TryEnqueue(extra));
}

void TestFormatter() {
    // 时间戳：epoch + 闰年（1972-02-29 = 第 789 天）
    char ts[32];
    mmo::core::detail::FormatTimestampNs(0, ts, sizeof(ts));
    CHECK(std::string(ts) == "1970-01-01T00:00:00.000000000Z");
    mmo::core::detail::FormatTimestampNs(static_cast<std::int64_t>(86400) * 1000000000LL, ts,
                                         sizeof(ts));
    CHECK(std::string(ts) == "1970-01-02T00:00:00.000000000Z");
    mmo::core::detail::FormatTimestampNs(static_cast<std::int64_t>(365) * 86400 * 1000000000LL, ts,
                                         sizeof(ts));
    CHECK(std::string(ts) == "1971-01-01T00:00:00.000000000Z");
    mmo::core::detail::FormatTimestampNs(static_cast<std::int64_t>(789) * 86400 * 1000000000LL, ts,
                                         sizeof(ts));
    CHECK(std::string(ts) == "1972-02-29T00:00:00.000000000Z");
    // 容量不足时不溢出
    char tiny[4];
    CHECK(mmo::core::detail::FormatTimestampNs(0, tiny, sizeof(tiny)) == 0);

    // 文本行：九项固定字段的 key 全部出现
    LogRecord rec{};
    rec.timestamp_ns = static_cast<std::int64_t>(789) * 86400 * 1000000000LL;
    rec.level = LogLevel::Warn;
    rec.thread_id = 3;
    rec.service = "gamenode";
    rec.module = "core.test";
    rec.trace_id = 0x0001000200030004ull;
    rec.request_id = 0x0001000200030005ull;
    rec.player_id = 42;
    rec.scene_id = 7;
    const std::string msg = "cast skill 17 on target 99";
    std::memcpy(rec.message, msg.c_str(), msg.size() + 1);
    rec.message_len = static_cast<std::uint32_t>(msg.size());
    rec.packed_size = static_cast<std::uint32_t>(PackedSize(rec));

    char text[mmo::core::detail::kFormatLineCapacity];
    mmo::core::detail::FormatTextLine(rec, text, sizeof(text));
    const std::string text_line(text);
    CHECK(text_line.find("1972-02-29T00:00:00.000000000Z") != std::string::npos);  // timestamp
    CHECK(text_line.find("WARN") != std::string::npos);                            // level
    CHECK(text_line.find("svc=gamenode") != std::string::npos);                    // service
    CHECK(text_line.find("mod=core.test") != std::string::npos);                   // module
    CHECK(text_line.find("trace=0001000200030004") != std::string::npos);          // trace_id
    CHECK(text_line.find("req=0001000200030005") != std::string::npos);            // request_id
    CHECK(text_line.find("player=42") != std::string::npos);                       // player_id
    CHECK(text_line.find("scene=7") != std::string::npos);                         // scene_id
    CHECK(text_line.find("tid=3") != std::string::npos);                           // thread_id
    CHECK(text_line.find("msg=cast skill 17 on target 99") != std::string::npos);  // message

    // JSON 行：九项字段的 key 齐全
    char json[mmo::core::detail::kFormatLineCapacity];
    mmo::core::detail::FormatJsonLine(rec, json, sizeof(json));
    const std::string json_line(json);
    const char* keys[] = {"\"ts_ns\"",     "\"level\"",     "\"service\"",    "\"module\"",
                          "\"trace_id\"",  "\"request_id\"", "\"player_id\"", "\"scene_id\"",
                          "\"thread_id\"", "\"message\""};
    for (const char* k : keys) {
        CHECK(json_line.find(k) != std::string::npos);
    }
    CHECK(json_line.find("0001000200030004") != std::string::npos);

    // JSON 转义
    LogRecord esc = rec;
    const std::string tricky = "say \"hi\"\nline2\\end";
    std::memcpy(esc.message, tricky.c_str(), tricky.size() + 1);
    esc.message_len = static_cast<std::uint32_t>(tricky.size());
    esc.packed_size = static_cast<std::uint32_t>(PackedSize(esc));
    mmo::core::detail::FormatJsonLine(esc, json, sizeof(json));
    const std::string escaped(json);
    CHECK(escaped.find("\\\"hi\\\"") != std::string::npos);
    CHECK(escaped.find("\\nline2") != std::string::npos);
    CHECK(escaped.find("\\\\end") != std::string::npos);

    // 未设置的 player / scene 在文本模式为 '-'，JSON 模式为 null
    LogRecord nil = rec;
    nil.player_id = kInvalidPlayerId;
    nil.scene_id = kInvalidSceneId;
    nil.packed_size = static_cast<std::uint32_t>(PackedSize(nil));
    mmo::core::detail::FormatTextLine(nil, text, sizeof(text));
    CHECK(std::string(text).find("player=-") != std::string::npos);
    CHECK(std::string(text).find("scene=-") != std::string::npos);
    mmo::core::detail::FormatJsonLine(nil, json, sizeof(json));
    CHECK(std::string(json).find("\"player_id\":null") != std::string::npos);
    CHECK(std::string(json).find("\"scene_id\":null") != std::string::npos);

    // 超长 message 截断且不溢出缓冲
    LogRecord huge = rec;
    huge.message_len = static_cast<std::uint32_t>(kMaxLogMessage);  // 故意越界，验证截断
    huge.packed_size = static_cast<std::uint32_t>(PackedSize(huge));
    CHECK(huge.packed_size <= sizeof(LogRecord));
}

// ===========================================================================
// §17 集成测试：跨线程 TraceID 串联 + 文件落盘反查
// ===========================================================================
void TestTracePropagationAndFile() {
    ResetTestDir();
    const std::string path = (TestDir() / "app.log").string();

    LoggerConfig cfg{};
    cfg.service = "gamenode";
    cfg.level = LogLevel::Trace;
    cfg.console = false;
    cfg.file_path = path;
    cfg.queue_capacity = 4096;
    auto init = Logger::Init(cfg);
    CHECK(init.HasValue());

    auto capture = std::make_shared<CaptureSink>();
    Logger::RegisterSink(capture);
    // 等待后台线程拾取新 sink 快照（RegisterSink 会 notify，这里留足调度余量）
    std::this_thread::sleep_for(std::chrono::milliseconds(20));

    const TraceID trace = NewTraceID();
    LogContext ctx{};
    ctx.trace_id = trace;
    ctx.request_id = DeriveRequestID(trace);
    ctx.player_id = 42;
    ctx.scene_id = 7;
    ctx.module = "core.test";
    ScopedLogContext scoped(ctx);

    MMO_LOG_INFO("main: receive cast skill request, skill={}", 17);

    // 跨线程：显式拷贝上下文，worker 内用 WithContext 套用（§17）
    const LogContext carried = CurrentLogContext();
    std::thread worker([carried] {
        WithContext(carried, [] {
            MMO_LOG_INFO("worker: validate target, ok");
            MMO_LOG_WARN("worker: target hp low, hp={}", 123);
        });
    });
    worker.join();

    Logger::Flush();
    Logger::Shutdown();

    // 1) 捕获到的记录：三条，TraceID 一致，字段完整，时间戳不倒退
    CHECK(capture->records.size() == 3);
    std::uint32_t thread_ids[3] = {0, 0, 0};
    for (std::size_t i = 0; i < capture->records.size() && i < 3; ++i) {
        const LogRecord& r = capture->records[i];
        CHECK(r.trace_id == trace);
        CHECK(r.player_id == 42);
        CHECK(r.scene_id == 7);
        CHECK(std::string(r.service) == "gamenode");
        CHECK(std::string(r.module) == "core.test");
        CHECK(r.message_len > 0);
        thread_ids[i] = r.thread_id;
    }
    if (capture->records.size() == 3) {
        CHECK(thread_ids[1] == thread_ids[2]);        // 两条 worker 日志同线程
        CHECK(thread_ids[0] != thread_ids[1]);        // 与主线程不同
        CHECK(capture->records[0].timestamp_ns <= capture->records[1].timestamp_ns);
        CHECK(capture->records[1].timestamp_ns <= capture->records[2].timestamp_ns);
    }

    // 2) 落盘文件：同一 TraceID 的日志可被完整检索（parse_trace.py 的输入格式）
    const std::string content = ReadFile(path);
    char trace_hex[32];
    std::snprintf(trace_hex, sizeof(trace_hex), "trace=%016llx",
                  static_cast<unsigned long long>(trace));
    CHECK(CountLinesContaining(content, trace_hex) == 3);
    CHECK(CountLinesContaining(content, "msg=main: receive cast skill request, skill=17") == 1);
    CHECK(CountLinesContaining(content, "player=42") == 3);

    // 3) 未初始化时的写入必须静默丢弃，不得崩溃
    MMO_LOG_INFO("after shutdown, this must be dropped silently");
    CHECK(true);
}

// ===========================================================================
// §17 集成测试：JSON 模式九字段齐全
// ===========================================================================
void TestJsonMode() {
    ResetTestDir();
    const std::string path = (TestDir() / "json.log").string();

    LoggerConfig cfg{};
    cfg.service = "dataservice";
    cfg.level = LogLevel::Trace;
    cfg.console = false;
    cfg.file_path = path;
    cfg.json = true;
    auto init = Logger::Init(cfg);
    CHECK(init.HasValue());

    LogContext ctx{};
    ctx.trace_id = NewTraceID();
    ctx.module = "data";
    ScopedLogContext scoped(ctx);
    MMO_LOG_ERROR("load player failed, id={}", 42);
    Logger::Flush();
    Logger::Shutdown();

    const std::string line = ReadFile(path);
    const char* keys[] = {"\"ts_ns\"",   "\"ts\"",       "\"level\"",     "\"service\"",
                          "\"module\"",  "\"trace_id\"", "\"request_id\"", "\"player_id\"",
                          "\"scene_id\"", "\"thread_id\"", "\"message\""};
    for (const char* k : keys) {
        CHECK(line.find(k) != std::string::npos);
    }
    CHECK(line.find("\"level\":\"ERROR\"") != std::string::npos);
    CHECK(line.find("\"service\":\"dataservice\"") != std::string::npos);
    CHECK(line.find("load player failed, id=42") != std::string::npos);
}

// ===========================================================================
// §17 集成测试：文件滚动后的数量与命名
// ===========================================================================
void TestRotation() {
    ResetTestDir();
    const std::string path = (TestDir() / "rot.log").string();

    LoggerConfig cfg{};
    cfg.level = LogLevel::Info;
    cfg.console = false;
    cfg.file_path = path;
    cfg.file_max_size = 1024;  // 约 8~10 行滚动一次
    cfg.file_max_files = 3;
    cfg.queue_capacity = 8192;
    auto init = Logger::Init(cfg);
    CHECK(init.HasValue());

    for (int i = 0; i < 2000; ++i) {
        MMO_LOG_INFO("rotation probe line {}", i);
    }
    Logger::Flush();
    Logger::Shutdown();

    CHECK(std::filesystem::exists(path));
    CHECK(std::filesystem::exists(path + ".1"));
    CHECK(std::filesystem::exists(path + ".2"));
    CHECK(std::filesystem::exists(path + ".3"));
    CHECK(!std::filesystem::exists(path + ".4"));  // 超出保留份数必须被删除
    CHECK(std::filesystem::file_size(path) <= 1024 + 256);
}

// ===========================================================================
// §19 失败测试：队列打满 —— 不阻塞、不崩溃、丢弃计数正确、产生一条 Warn
// ===========================================================================
void TestQueueOverflow() {
    LoggerConfig cfg{};
    cfg.level = LogLevel::Info;
    cfg.console = false;
    cfg.queue_capacity = 256;  // 故意开小，确保必然打满
    auto init = Logger::Init(cfg);
    CHECK(init.HasValue());

    // 后台线程首条记录阻塞 200ms，模拟「后台线程被卡住」（§19）
    auto capture = std::make_shared<CaptureSink>(/*store_all=*/false, /*block_once_ms=*/200);
    Logger::RegisterSink(capture);

    const std::uint64_t dropped_before = Logger::DroppedCount();

    constexpr int kThreads = 4;
    constexpr int kPerThread = 5000;
    std::vector<std::thread> pool;
    for (int t = 0; t < kThreads; ++t) {
        pool.emplace_back([t] {
            for (int i = 0; i < kPerThread; ++i) {
                MMO_LOG_INFO("overflow probe thread={} i={}", t, i);
            }
        });
    }
    const auto begin = std::chrono::steady_clock::now();
    for (auto& th : pool) {
        th.join();
    }
    // 业务线程不被阻塞：4×5000 条在后台线程被卡住的情况下仍必须快速返回
    const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now() - begin)
                                .count();
    CHECK(elapsed_ms < 3000);

    Logger::Flush();
    const std::uint64_t dropped = Logger::DroppedCount() - dropped_before;
    CHECK(dropped > 0);                                     // 确实发生了丢弃
    CHECK(Logger::EnqueuedCount() > 0);                     // 队列恢复后仍有日志成功入队
    CHECK(capture->WarnTotal() >= 1);                       // 至少一条溢出告警
    CHECK(capture->WarnTotal() <= 4);                       // 告警按突发收敛，不刷屏
    Logger::Shutdown();
}

// ===========================================================================
// §19 失败测试：日志文件打不开 —— Init 返回 Error 而不是崩溃
// ===========================================================================
void TestInitFailure() {
    // 目录不存在（与「只读目录」走同一条 fopen 失败路径）
    LoggerConfig bad{};
    bad.console = false;
    bad.file_path = (TestDir() / "no_such_dir" / "app.log").string();
    auto r = Logger::Init(bad);
    CHECK(!r.HasValue());
    if (!r.HasValue()) {
        CHECK(r.Err().Code() == ErrorCode::INTERNAL_ERROR);
        CHECK(r.Err().Message().find("open log file failed") != std::string::npos);
    }
    CHECK(!Logger::IsInitialized());
    // 失败后写入必须静默丢弃
    MMO_LOG_INFO("must be dropped");
    Logger::Shutdown();  // 未初始化时 Shutdown 应为 no-op

    // 重复初始化：第二次必须失败而不是重启线程
    ResetTestDir();
    LoggerConfig ok{};
    ok.console = false;
    ok.file_path = (TestDir() / "twice.log").string();
    CHECK(Logger::Init(ok).HasValue());
    auto second = Logger::Init(ok);
    CHECK(!second.HasValue());
    if (!second.HasValue()) {
        CHECK(second.Err().Code() == ErrorCode::BUSY);
    }
    Logger::Shutdown();
}

// ===========================================================================
// §19 失败测试：Fatal 写入后立即落盘（业务线程不显式调用 Flush）
// ===========================================================================
void TestFatalFlush() {
    ResetTestDir();
    const std::string path = (TestDir() / "fatal.log").string();
    LoggerConfig cfg{};
    cfg.console = false;
    cfg.file_path = path;
    CHECK(Logger::Init(cfg).HasValue());

    MMO_LOG_FATAL("fatal happened, code={}", -1);
    // 不调用 Logger::Flush()：Fatal 必须自己保证已落盘
    const std::string content = ReadFile(path);
    CHECK(content.find("fatal happened, code=-1") != std::string::npos);
    CHECK(content.find("FATAL") != std::string::npos);
    Logger::Shutdown();
}

// ===========================================================================
// §20 验收：关闭日志时 MMO_LOG 的开销（bench 里有精确阈值，这里只防回归）
// ===========================================================================
void TestDisabledCost() {
    LoggerConfig cfg{};
    cfg.console = false;
    cfg.queue_capacity = 4096;
    CHECK(Logger::Init(cfg).HasValue());
    Logger::SetLevel(LogLevel::Fatal);  // 关闭 Info 及以下

    CHECK(!Logger::ShouldLog(LogLevel::Info));
    CHECK(Logger::ShouldLog(LogLevel::Fatal));

    constexpr int kIterations = 1000000;
    volatile int sink = 0;
    const auto begin = std::chrono::steady_clock::now();
    for (int i = 0; i < kIterations; ++i) {
        MMO_LOG_INFO("disabled path {}", i);
        sink = i;  // 防止整个循环被优化掉
    }
    const double ns = static_cast<double>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() -
                                                             begin)
            .count()) / static_cast<double>(kIterations);
    (void)sink;
    ::mmo::core::test::ErrorFmt("[info] disabled MMO_LOG cost = %.3f ns/call\n", ns);
    CHECK(ns < 1000.0);  // Debug 构建下阈值放宽，精确阈值由 bench/core_log.txt 断言

    Logger::SetLevel(LogLevel::Info);
    Logger::Shutdown();
}

// ===========================================================================
// §22：单条日志零堆分配
// ===========================================================================
void TestZeroAllocation() {
    LoggerConfig cfg{};
    cfg.console = false;
    cfg.queue_capacity = 4096;
    CHECK(Logger::Init(cfg).HasValue());
    auto counter = std::make_shared<CaptureSink>(false);
    Logger::RegisterSink(counter);

    LogContext ctx{};
    ctx.trace_id = NewTraceID();
    ctx.module = "core.test";
    ctx.player_id = 42;
    ScopedLogContext scoped(ctx);

    // 预热：首条日志会走一次惰性初始化路径
    for (int i = 0; i < 100; ++i) {
        MMO_LOG_INFO("warmup {}", i);
    }
    Logger::Flush();

    const std::size_t before = g_alloc_count;
    for (int i = 0; i < 1000; ++i) {
        MMO_LOG_INFO("alloc probe i={} player={}", i, 42);
    }
    Logger::Flush();
    const std::size_t delta = g_alloc_count - before;
    ::mmo::core::test::ErrorFmt("[info] heap allocations for 1000 log calls = %zu\n", delta);
    CHECK(delta == 0);

    Logger::Shutdown();
}

// ===========================================================================
// §17 + §20.3 集成测试：落盘探针，供 tools/logtrace/parse_trace.py 真实检索
//
// 前面的用例只校验「内存里捕获到的记录」，这里额外把它写成仓库内的
// bench/trace_probe.log，让验收人能真跑一次 parse_trace.py（§20.3 是人工复核项，
// 必须有可复现的输入，而不是只在测试内存里 grep）。
// ===========================================================================
void TestTraceProbe() {
    std::filesystem::create_directories("bench");
    const std::string path = "bench/trace_probe.log";
    std::filesystem::remove(path);  // 每次重跑保持干净，避免多次运行叠加

    LoggerConfig cfg{};
    cfg.service = "gamenode";
    cfg.level = LogLevel::Trace;
    cfg.console = false;
    cfg.file_path = path;
    cfg.queue_capacity = 4096;
    CHECK(Logger::Init(cfg).HasValue());

    const TraceID trace = NewTraceID();
    LogContext ctx{};
    ctx.trace_id = trace;
    ctx.request_id = DeriveRequestID(trace);
    ctx.player_id = 42;
    ctx.scene_id = 7;
    ctx.module = "core.test";
    ScopedLogContext scoped(ctx);

    MMO_LOG_INFO("probe: main thread begin");
    const LogContext carried = CurrentLogContext();
    std::thread worker([carried] {
        WithContext(carried, [] {
            MMO_LOG_INFO("probe: worker validate");
            MMO_LOG_WARN("probe: worker done, cost_ms={}", 3);
        });
    });
    worker.join();
    MMO_LOG_INFO("probe: main thread end");
    Logger::Flush();
    Logger::Shutdown();

    // 落盘断言 + 把 TraceID 打出来，便于人工执行：
    //   python tools/logtrace/parse_trace.py <trace> bench/trace_probe.log
    const std::string content = ReadFile(path);
    char trace_hex[32];
    std::snprintf(trace_hex, sizeof(trace_hex), "%016llx",
                  static_cast<unsigned long long>(trace));
    CHECK(CountLinesContaining(content, trace_hex) == 4);
    ::mmo::core::test::ErrorFmt("[info] trace_probe: file=%s trace=%s\n", path.c_str(),
                                trace_hex);
}

}  // namespace

int main() {
    TestLogLevel();
    TestTraceId();
    TestLogContext();
    TestRingBuffer();
    TestFormatter();

    TestTracePropagationAndFile();
    TestJsonMode();
    TestRotation();
    TestTraceProbe();
    TestQueueOverflow();
    TestInitFailure();
    TestFatalFlush();
    TestDisabledCost();
    TestZeroAllocation();

    if (failures == 0) {
        ::mmo::core::test::Error("===== TASK-002 log_test: ALL PASSED =====\n");
        return 0;
    }
    ::mmo::core::test::ErrorFmt("===== TASK-002 log_test: %d FAILURE(S) =====\n", failures);
    return 1;
}
