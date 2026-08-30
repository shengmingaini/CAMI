#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

#include "mmo/core/error/error.h"
#include "mmo/core/error/result.h"

namespace mmo::core {

namespace detail {
class ConfigSnapshot;  ///< 不可变快照，定义不对外暴露（#include 不到，也就改不了）
}  // namespace detail

/// ConfigManager —— 进程级配置中心（JSON）。
///
/// State Owner  : 配置快照不可变。写入权归 ControlService / 运维线程（加载期与热更期），
///                运行期所有业务线程只读，版本切换通过原子指针整体替换（禁止原地修改）。
/// Thread Safety: 读路径无锁、无分配（任务书 §9）；写路径（Load*/Set/Reload）用互斥串起来，
///                但**不在**热路径上——Tick 循环内禁止读取配置文件（任务书 §21）。
/// Hot Path     : Get<T> 位于热路径读侧，实测 < 50ns（见 docs/PERFORMANCE.md）。
///
/// 关键不变量：
///   1. 读者永远看到「某个完整版本的快照」，绝不会读到半更新状态。
///   2. Load*/Reload 失败时保留旧快照并返回 Error，绝不留下半替换状态（任务书 §15.7）。
///   3. 每次成功变更 Version() +1，读者可用它判断配置是否已切换。
///
/// 禁止：把 Get 返回的裸指针/引用长期持有（Get<T> 返回值语义，天然安全）；
///       在 Tick 循环里调用 Load*/Reload（外部 IO，必须异步化）。
class ConfigManager {
public:
    /// 加载单个 JSON 文件并**合并**进当前快照；同时登记为 Reload 源。
    /// 文件不存在 -> NOT_FOUND；JSON 非法 -> INVALID_ARGUMENT；键冲突 -> INVALID_ARGUMENT。
    static Result<void> LoadFile(std::string_view path);

    /// 加载目录下所有 *.json（按文件名排序）并合并；同时登记为 Reload 源。
    /// 目录不存在 -> NOT_FOUND。
    static Result<void> LoadDir(std::string_view dir);

    /// 读取配置项。
    ///   key 不存在   -> ErrorCode::NOT_FOUND（message 带 key 名）
    ///   类型不匹配   -> ErrorCode::INVALID_ARGUMENT
    /// 支持 T：bool / int32_t / uint32_t / int64_t / uint64_t / double / std::string
    template <typename T>
    static Result<T> Get(std::string_view key);

    /// 运行期覆盖（**仅测试用**）：基于当前快照复制一份并原子替换，Version +1。
    /// 注意：下一次 Reload 会重新读文件，覆盖值会被丢弃。
    static Result<void> Set(std::string_view key, std::string value);

    /// 当前快照版本号，每次成功变更 +1（首个空快照为 0）。
    static std::uint64_t Version() noexcept;

    /// 重新加载所有已登记的源，成功后原子替换快照。
    /// 任一源失败 -> 保留旧快照并返回 Error，**绝不半替换**。
    static Result<void> Reload();

    static bool Contains(std::string_view key);

    /// 稳定顺序的键列表（插入顺序），便于运维核对与测试断言。
    static std::vector<std::string> Keys();

    static std::size_t Size();

    /// 仅用于测试隔离：清空快照与已登记源，版本归零。生产路径禁止调用。
    static void ResetForTest() noexcept;

private:
    /// 取得当前快照的**裸指针**。所有权由线程局部缓存中的强引用持有，
    /// 因此返回的指针在本次调用内一定有效；这是内部优化，不对外暴露。
    static const detail::ConfigSnapshot* AcquireSnapshot() noexcept;

    /// 在指定快照内查找；不存在返回 nullptr。
    static const std::string* FindInSnapshot(const detail::ConfigSnapshot& snap,
                                             std::string_view key) noexcept;
};

namespace detail {

template <typename>
struct AlwaysFalse : std::false_type {};

/// 配置值转换器。主模板故意不提供 Parse：不支持的类型在**编译期**报错，
/// 而不是运行期返回莫名其妙的错误码。
template <typename T>
struct ConfigValue {
    static_assert(AlwaysFalse<T>::value,
                  "ConfigManager::Get<T> 只支持 bool / int32_t / uint32_t / int64_t / "
                  "uint64_t / double / std::string");
};

template <>
struct ConfigValue<std::string> {
    static Result<std::string> Parse(std::string_view raw, std::string_view key);
};
template <>
struct ConfigValue<bool> {
    static Result<bool> Parse(std::string_view raw, std::string_view key);
};
template <>
struct ConfigValue<std::int64_t> {
    static Result<std::int64_t> Parse(std::string_view raw, std::string_view key);
};
template <>
struct ConfigValue<std::uint64_t> {
    static Result<std::uint64_t> Parse(std::string_view raw, std::string_view key);
};
template <>
struct ConfigValue<std::int32_t> {
    static Result<std::int32_t> Parse(std::string_view raw, std::string_view key);
};
template <>
struct ConfigValue<std::uint32_t> {
    static Result<std::uint32_t> Parse(std::string_view raw, std::string_view key);
};
template <>
struct ConfigValue<double> {
    static Result<double> Parse(std::string_view raw, std::string_view key);
};

/// 统一的「类型不匹配」错误构造：message 形如 "bad int64: tick.hz"。
Error MakeConfigTypeError(std::string_view type_name, std::string_view key);

}  // namespace detail

template <typename T>
Result<T> ConfigManager::Get(std::string_view key) {
    const detail::ConfigSnapshot* snapshot = AcquireSnapshot();
    const std::string* raw = FindInSnapshot(*snapshot, key);
    if (raw == nullptr) {
        std::string message = "no such key: ";
        message.append(key);
        return Result<T>::Fail(Error(ErrorCode::NOT_FOUND, message));
    }
    return detail::ConfigValue<T>::Parse(*raw, key);
}

}  // namespace mmo::core
