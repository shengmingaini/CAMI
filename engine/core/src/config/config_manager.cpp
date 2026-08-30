#include "mmo/core/config/config_manager.h"

#include <algorithm>
#include <atomic>
#include <charconv>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <utility>

#include "config/config_snapshot.h"
#include "config/json_parser.h"
#include "mmo/core/error/error.h"

namespace mmo::core {
namespace {

/// 读缓存的「无效版本」哨兵：确保每个线程首次 Get 一定去取一次真实快照。
constexpr std::uint64_t kInvalidCacheVersion = ~std::uint64_t{0};

struct ManagerState {
    /// 只保护写入路径（Load*/Set/Reload）。读路径完全不碰它——冷路径锁，不是全局锁。
    std::mutex write_mutex;
    /// 当前快照（原子整体替换）。读者通过线程局部缓存持有强引用后只读裸指针。
    std::atomic<std::shared_ptr<const detail::ConfigSnapshot>> current;
    /// 与 current 配套的单调版本；读路径唯一需要碰的原子量。
    std::atomic<std::uint64_t> version{0};
    std::vector<std::string> source_files;
    std::vector<std::string> source_dirs;

    ManagerState() {
        auto empty = std::make_shared<const detail::ConfigSnapshot>();
        current.store(empty, std::memory_order_release);
    }
};

ManagerState& State() {
    static ManagerState state;
    return state;
}

/// 线程局部读缓存：持有最近一次快照的强引用，
/// 使 Get 的快速路径只需一次 relaxed/acquire 的 uint64 原子读 + 一次裸指针比较。
///
/// 为什么需要它：直接对 std::atomic<shared_ptr> 做 load 会动引用计数（两次原子 RMW），
/// 实测把 config_get_ns 推到阈值边缘。缓存后读路径零 RMW。
struct ThreadCache {
    std::shared_ptr<const detail::ConfigSnapshot> snapshot;
    std::uint64_t version = kInvalidCacheVersion;
};

ThreadCache& Cache() {
    static thread_local ThreadCache cache;
    return cache;
}

Result<std::string> ReadTextFile(std::string_view path) {
    std::ifstream input(std::string(path), std::ios::binary);
    if (!input.is_open()) {
        std::string message = "cannot open: ";
        message.append(path);
        return Result<std::string>::Fail(Error(ErrorCode::NOT_FOUND, message));
    }
    std::ostringstream buffer;
    buffer << input.rdbuf();
    if (input.bad()) {
        return Result<std::string>::Fail(Error(ErrorCode::INTERNAL_ERROR, "config: read failed"));
    }
    return Result<std::string>::Ok(buffer.str());
}

/// 把一个 JSON 文本解析成扁平表并合并进 accumulated；重复键交给 Build 阶段判定。
Result<void> ParseInto(config_internal::FlatMap& accumulated, std::string_view text) {
    config_internal::FlatMap flat;
    const Result<void> parsed = config_internal::ParseJson(text, flat);
    if (!parsed.HasValue()) {
        return Result<void>::Fail(parsed.Err());
    }
    accumulated.insert(accumulated.end(), std::make_move_iterator(flat.begin()),
                       std::make_move_iterator(flat.end()));
    return Result<void>::Ok();
}

Result<void> ParseFileInto(config_internal::FlatMap& accumulated, std::string_view path) {
    const Result<std::string> content = ReadTextFile(path);
    if (!content.HasValue()) {
        return Result<void>::Fail(content.Err());
    }
    return ParseInto(accumulated, content.Value());
}

/// 发布新快照：先落快照（内含自身版本），再抬全局版本。
/// 顺序保证读者取到的快照版本 >= 它读到的全局版本，缓存判定因此永远不陈旧。
void PublishSnapshot(std::shared_ptr<detail::ConfigSnapshot> next) {
    ManagerState& state = State();
    const std::uint64_t next_version = state.version.load(std::memory_order_relaxed) + 1;
    next->SetVersion(next_version);
    std::shared_ptr<const detail::ConfigSnapshot> frozen = std::move(next);
    state.current.store(frozen, std::memory_order_release);
    state.version.store(next_version, std::memory_order_release);
}

bool EqualsIgnoreAsciiCase(std::string_view a, std::string_view b) noexcept {
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        const char ca = a[i];
        const char cb = b[i];
        const bool lower_a = (ca >= 'A' && ca <= 'Z');
        const bool lower_b = (cb >= 'A' && cb <= 'Z');
        if (lower_a != lower_b) {
            return false;
        }
        if (lower_a) {
            if ((ca | 0x20) != (cb | 0x20)) {
                return false;
            }
            continue;
        }
        if (ca != cb) {
            return false;
        }
    }
    return true;
}

}  // namespace

// ---- 读路径 -------------------------------------------------------------

const detail::ConfigSnapshot* ConfigManager::AcquireSnapshot() noexcept {
    ManagerState& state = State();
    ThreadCache& cache = Cache();
    const std::uint64_t version = state.version.load(std::memory_order_acquire);
    if ((cache.version != version) || (cache.snapshot == nullptr)) {
        cache.snapshot = state.current.load(std::memory_order_acquire);
        if (cache.snapshot == nullptr) {
            cache.snapshot = std::make_shared<const detail::ConfigSnapshot>();
        }
        // 用快照自带版本记账：它可能比刚读到的全局版本更新（期间又热更了），
        // 但绝不会更旧，因此缓存永远不陈旧。
        cache.version = cache.snapshot->Version();
    }
    return cache.snapshot.get();
}

const std::string* ConfigManager::FindInSnapshot(const detail::ConfigSnapshot& snap,
                                                 std::string_view key) noexcept {
    return snap.Find(key);
}

std::uint64_t ConfigManager::Version() noexcept {
    return State().version.load(std::memory_order_acquire);
}

bool ConfigManager::Contains(std::string_view key) {
    return AcquireSnapshot()->Contains(key);
}

std::vector<std::string> ConfigManager::Keys() {
    return AcquireSnapshot()->Keys();
}

std::size_t ConfigManager::Size() {
    return AcquireSnapshot()->Size();
}

// ---- 写路径（冷路径，互斥串行） ----------------------------------------

Result<void> ConfigManager::LoadFile(std::string_view path) {
    config_internal::FlatMap flat;
    const Result<void> parsed = ParseFileInto(flat, path);
    if (!parsed.HasValue()) {
        return Result<void>::Fail(parsed.Err());
    }
    const Result<std::shared_ptr<detail::ConfigSnapshot>> built =
        detail::ConfigSnapshot::Build(flat);
    if (!built.HasValue()) {
        return Result<void>::Fail(built.Err());
    }

    ManagerState& state = State();
    std::lock_guard<std::mutex> lock(state.write_mutex);
    auto merged = state.current.load(std::memory_order_acquire)->Cloned();
    merged->MergeFrom(*built.Value());
    const std::string key(path);
    if (std::find(state.source_files.begin(), state.source_files.end(), key) ==
        state.source_files.end()) {
        state.source_files.push_back(key);
    }
    PublishSnapshot(std::move(merged));
    return Result<void>::Ok();
}

Result<void> ConfigManager::LoadDir(std::string_view dir) {
    std::error_code ec;
    const std::filesystem::path root(dir);
    if (!std::filesystem::is_directory(root, ec)) {
        std::string message = "no such dir: ";
        message.append(dir);
        return Result<void>::Fail(Error(ErrorCode::NOT_FOUND, message));
    }

    std::vector<std::filesystem::path> files;
    for (const auto& entry : std::filesystem::directory_iterator(root, ec)) {
        if (ec) {
            break;
        }
        if (entry.is_regular_file() && entry.path().extension() == ".json") {
            files.push_back(entry.path());
        }
    }
    if (ec) {
        return Result<void>::Fail(Error(ErrorCode::INTERNAL_ERROR, "config: dir read failed"));
    }
    std::sort(files.begin(), files.end());

    config_internal::FlatMap flat;
    for (const auto& file : files) {
        const Result<void> parsed = ParseFileInto(flat, file.string());
        if (!parsed.HasValue()) {
            return Result<void>::Fail(parsed.Err());
        }
    }
    const Result<std::shared_ptr<detail::ConfigSnapshot>> built =
        detail::ConfigSnapshot::Build(flat);
    if (!built.HasValue()) {
        return Result<void>::Fail(built.Err());
    }

    ManagerState& state = State();
    std::lock_guard<std::mutex> lock(state.write_mutex);
    auto merged = state.current.load(std::memory_order_acquire)->Cloned();
    merged->MergeFrom(*built.Value());
    const std::string key(dir);
    if (std::find(state.source_dirs.begin(), state.source_dirs.end(), key) ==
        state.source_dirs.end()) {
        state.source_dirs.push_back(key);
    }
    PublishSnapshot(std::move(merged));
    return Result<void>::Ok();
}

Result<void> ConfigManager::Reload() {
    ManagerState& state = State();
    std::lock_guard<std::mutex> lock(state.write_mutex);

    // 从空快照重建：任一源失败立即返回，旧快照原样保留（禁止半替换）。
    config_internal::FlatMap flat;
    for (const std::string& file : state.source_files) {
        const Result<void> parsed = ParseFileInto(flat, file);
        if (!parsed.HasValue()) {
            return Result<void>::Fail(parsed.Err());
        }
    }
    for (const std::string& dir : state.source_dirs) {
        std::error_code ec;
        std::vector<std::filesystem::path> files;
        for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
            if (ec) {
                break;
            }
            if (entry.is_regular_file() && entry.path().extension() == ".json") {
                files.push_back(entry.path());
            }
        }
        if (ec) {
            return Result<void>::Fail(Error(ErrorCode::NOT_FOUND, "config: dir gone"));
        }
        std::sort(files.begin(), files.end());
        for (const auto& file : files) {
            const Result<void> parsed = ParseFileInto(flat, file.string());
            if (!parsed.HasValue()) {
                return Result<void>::Fail(parsed.Err());
            }
        }
    }

    const Result<std::shared_ptr<detail::ConfigSnapshot>> built =
        detail::ConfigSnapshot::Build(flat);
    if (!built.HasValue()) {
        return Result<void>::Fail(built.Err());
    }
    PublishSnapshot(built.Value());
    return Result<void>::Ok();
}

Result<void> ConfigManager::Set(std::string_view key, std::string value) {
    ManagerState& state = State();
    std::lock_guard<std::mutex> lock(state.write_mutex);
    const std::shared_ptr<const detail::ConfigSnapshot> base =
        state.current.load(std::memory_order_acquire);
    if (base == nullptr) {
        return Result<void>::Fail(Error(ErrorCode::INTERNAL_ERROR, "config: not ready"));
    }
    auto next = base->Cloned();
    next->Insert(std::string(key), std::move(value));
    PublishSnapshot(std::move(next));
    return Result<void>::Ok();
}

void ConfigManager::ResetForTest() noexcept {
    ManagerState& state = State();
    std::lock_guard<std::mutex> lock(state.write_mutex);
    state.source_files.clear();
    state.source_dirs.clear();
    // 版本号保持单调（不归零）：这样其它线程残留的读缓存一定会失配并刷新，
    // 避免「reset 后仍有线程握着旧快照」的测试隔离问题。
    auto empty = std::make_shared<detail::ConfigSnapshot>();
    PublishSnapshot(std::move(empty));
    Cache().snapshot.reset();
    Cache().version = kInvalidCacheVersion;
}

// ---- 值转换（类型不匹配 -> INVALID_ARGUMENT） ---------------------------

namespace detail {

Error MakeConfigTypeError(std::string_view type_name, std::string_view key) {
    std::string message = "bad ";
    message.append(type_name);
    message += ": ";
    message.append(key);
    return Error(ErrorCode::INVALID_ARGUMENT, message);
}

Result<std::string> ConfigValue<std::string>::Parse(std::string_view raw, std::string_view) {
    return Result<std::string>::Ok(std::string(raw));
}

Result<bool> ConfigValue<bool>::Parse(std::string_view raw, std::string_view key) {
    if (EqualsIgnoreAsciiCase(raw, "true") || raw == "1" || EqualsIgnoreAsciiCase(raw, "yes") ||
        EqualsIgnoreAsciiCase(raw, "on")) {
        return Result<bool>::Ok(true);
    }
    if (EqualsIgnoreAsciiCase(raw, "false") || raw == "0" || EqualsIgnoreAsciiCase(raw, "no") ||
        EqualsIgnoreAsciiCase(raw, "off")) {
        return Result<bool>::Ok(false);
    }
    return Result<bool>::Fail(MakeConfigTypeError("bool", key));
}

Result<std::int64_t> ConfigValue<std::int64_t>::Parse(std::string_view raw,
                                                      std::string_view key) {
    std::int64_t value = 0;
    const char* const begin = raw.data();
    const char* const end = begin + raw.size();
    const auto result = std::from_chars(begin, end, value);
    if (result.ec != std::errc{} || result.ptr != end) {
        return Result<std::int64_t>::Fail(MakeConfigTypeError("int64", key));
    }
    return Result<std::int64_t>::Ok(value);
}

Result<std::uint64_t> ConfigValue<std::uint64_t>::Parse(std::string_view raw,
                                                        std::string_view key) {
    std::uint64_t value = 0;
    const char* const begin = raw.data();
    const char* const end = begin + raw.size();
    const auto result = std::from_chars(begin, end, value);
    if (result.ec != std::errc{} || result.ptr != end) {
        return Result<std::uint64_t>::Fail(MakeConfigTypeError("uint64", key));
    }
    return Result<std::uint64_t>::Ok(value);
}

Result<std::int32_t> ConfigValue<std::int32_t>::Parse(std::string_view raw,
                                                      std::string_view key) {
    const Result<std::int64_t> wide = ConfigValue<std::int64_t>::Parse(raw, key);
    if (!wide.HasValue()) {
        return Result<std::int32_t>::Fail(wide.Err());
    }
    const std::int64_t value = wide.Value();
    if (value < INT32_MIN || value > INT32_MAX) {
        return Result<std::int32_t>::Fail(MakeConfigTypeError("int32", key));
    }
    return Result<std::int32_t>::Ok(static_cast<std::int32_t>(value));
}

Result<std::uint32_t> ConfigValue<std::uint32_t>::Parse(std::string_view raw,
                                                        std::string_view key) {
    const Result<std::uint64_t> wide = ConfigValue<std::uint64_t>::Parse(raw, key);
    if (!wide.HasValue()) {
        return Result<std::uint32_t>::Fail(wide.Err());
    }
    const std::uint64_t value = wide.Value();
    if (value > UINT32_MAX) {
        return Result<std::uint32_t>::Fail(MakeConfigTypeError("uint32", key));
    }
    return Result<std::uint32_t>::Ok(static_cast<std::uint32_t>(value));
}

Result<double> ConfigValue<double>::Parse(std::string_view raw, std::string_view key) {
    double value = 0.0;
    const char* const begin = raw.data();
    const char* const end = begin + raw.size();
    const auto result = std::from_chars(begin, end, value);
    if (result.ec != std::errc{} || result.ptr != end) {
        return Result<double>::Fail(MakeConfigTypeError("double", key));
    }
    return Result<double>::Ok(value);
}

}  // namespace detail

}  // namespace mmo::core
