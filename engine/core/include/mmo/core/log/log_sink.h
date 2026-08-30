#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

#include "mmo/core/error/result.h"
#include "mmo/core/log/log_record.h"

namespace mmo::core {

/// 输出格式。kText 人读（tools/logtrace/parse_trace.py 亦按此格式解析），
/// kJson 机读（结构化采集 / ELK 接入）。
enum class LogFormat : std::uint8_t {
    kText = 0,
    kJson = 1,
};

/// 日志输出目标抽象（§7 Public Interface）。
///
/// 线程契约（§11 / §21）：Write / Flush **只会被 Logger 的后台刷盘线程调用**，
/// 且是单线程串行调用，因此实现内部无需加锁、无需考虑重入。
/// 业务线程禁止直接调用 sink——那会把文件 IO 引入 Tick 热路径。
class ILogSink {
public:
    virtual ~ILogSink() = default;

    /// 写一条日志。必须为 noexcept：日志失败不得影响业务。
    virtual void Write(const LogRecord& record) noexcept = 0;

    /// 把已写入的数据刷到操作系统（默认无操作，供内存型 sink 忽略）。
    virtual void Flush() noexcept {}
};

/// 控制台 sink（开发用）。输出走 stdout 的 FILE* 而不是 std::cout：
/// ① 与 sink 的其它实现统一用 C 流式接口，便于批量 fwrite；
/// ② 遵守 §21 红线（全仓禁止 std::cout / printf 直接输出）。
///
/// color=true 时输出 ANSI 颜色，并在 Windows 上尝试开启虚拟终端处理；
/// 非字符设备（重定向到文件 / 管道）时自动关闭颜色。
std::shared_ptr<ILogSink> MakeConsoleSink(bool color = true,
                                          LogFormat format = LogFormat::kText);

/// 按大小滚动的文件 sink（生产用）。
///
/// 命名：`<path>` 为当前文件，`<path>.1` 为上一份，`<path>.2` 更早，最多保留 max_files 份。
/// 打开失败（目录不存在 / 只读 / 权限不足）返回 Error，**不崩溃、不抛异常**（§19）。
///
/// @param path       日志文件路径，空串直接返回 INVALID_ARGUMENT
/// @param max_size   单个文件字节上限，达到后滚动；0 视为 1 字节（兜底，避免死循环）
/// @param max_files  保留的历史文件份数，0 视为 1
Result<std::shared_ptr<ILogSink>> MakeRotatingFileSink(std::string path,
                                                       std::size_t max_size = 64u * 1024u * 1024u,
                                                       std::size_t max_files = 5,
                                                       LogFormat format = LogFormat::kText);

}  // namespace mmo::core
