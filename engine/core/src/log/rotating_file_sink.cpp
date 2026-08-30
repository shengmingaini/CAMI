// engine/core/src/log/rotating_file_sink.cpp — TASK-002 按大小滚动的文件 sink
//
// 契约：本文件的 Write / Flush 只会被 Logger 的后台线程调用，因此无需加锁（§9）。
// 打开失败返回 Error 而不崩溃（§19）：目录不存在 / 只读 / 权限不足均走同一条失败路径。

#include "mmo/core/log/log_sink.h"

#include <cstdio>
#include <string>
#include <utility>

#include "mmo/core/error/error.h"
#include "log/log_formatter.h"

namespace mmo::core {
namespace {

std::size_t FileSizeOf(const std::string& path) noexcept {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (f == nullptr) {
        return 0;
    }
    std::fseek(f, 0, SEEK_END);
    const long n = std::ftell(f);
    std::fclose(f);
    return n > 0 ? static_cast<std::size_t>(n) : 0;
}

class RotatingFileSink final : public ILogSink {
public:
    RotatingFileSink(std::string path, std::size_t max_size, std::size_t max_files,
                     LogFormat format)
        // 0 视为 1：max_size=0 会导致每条日志滚动一次，max_files=0 会丢掉全部历史。
        : path_(std::move(path)),
          max_size_(max_size == 0 ? 1 : max_size),
          max_files_(max_files == 0 ? 1 : max_files),
          format_(format) {}

    ~RotatingFileSink() override { Close(); }

    Result<void> Open() {
        if (path_.empty()) {
            return Result<void>::Fail(
                Error(ErrorCode::INVALID_ARGUMENT, "empty log file path", domain::kCore));
        }
        fp_ = std::fopen(path_.c_str(), "ab");
        if (fp_ == nullptr) {
            // 目录不存在 / 只读 / 权限不足：返回错误，交由调用方决定降级策略。
            const std::string msg = std::string("open log file failed: ") + path_;
            return Result<void>::Fail(
                Error(ErrorCode::INTERNAL_ERROR, msg, domain::kCore));
        }
        written_ = FileSizeOf(path_);
        return Result<void>::Ok();
    }

    void Write(const LogRecord& rec) noexcept override {
        if (fp_ == nullptr) {
            return;
        }
        char line[detail::kFormatLineCapacity + 1];
        const std::size_t n = (format_ == LogFormat::kJson)
                                  ? detail::FormatJsonLine(rec, line, sizeof(line) - 1)
                                  : detail::FormatTextLine(rec, line, sizeof(line) - 1);
        std::size_t len = n;
        if (len < sizeof(line) - 1) {
            line[len++] = '\n';
        }
        written_ += std::fwrite(line, 1, len, fp_);
        if (written_ >= max_size_) {
            Rotate();
        }
    }

    void Flush() noexcept override {
        if (fp_ != nullptr) {
            std::fflush(fp_);
        }
    }

private:
    void Close() noexcept {
        if (fp_ != nullptr) {
            std::fclose(fp_);
            fp_ = nullptr;
        }
    }

    /// 滚动：删最旧 -> 历史整体后移 -> 当前文件变成 .1 -> 新建当前文件。
    /// 命名固定为 `<path>`（当前）+ `<path>.1` .. `<path>.<N>`（由新到旧）。
    void Rotate() noexcept {
        Close();
        std::remove((path_ + "." + std::to_string(max_files_)).c_str());
        for (std::size_t i = max_files_; i > 1; --i) {
            const std::string from = path_ + "." + std::to_string(i - 1);
            const std::string to = path_ + "." + std::to_string(i);
            // 目标位置必为空（已由上一轮后移腾出），因此 Windows 的 rename 也能成功。
            std::rename(from.c_str(), to.c_str());
        }
        std::rename(path_.c_str(), (path_ + ".1").c_str());
        fp_ = std::fopen(path_.c_str(), "ab");
        written_ = 0;
    }

    std::string path_;
    std::size_t max_size_;
    std::size_t max_files_;
    LogFormat format_;
    std::FILE* fp_{nullptr};
    std::size_t written_{0};
};

}  // namespace

Result<std::shared_ptr<ILogSink>> MakeRotatingFileSink(std::string path, std::size_t max_size,
                                                       std::size_t max_files, LogFormat format) {
    auto sink =
        std::make_shared<RotatingFileSink>(std::move(path), max_size, max_files, format);
    auto opened = sink->Open();
    if (!opened) {
        return Result<std::shared_ptr<ILogSink>>::Fail(std::move(opened).Err());
    }
    return Result<std::shared_ptr<ILogSink>>::Ok(std::shared_ptr<ILogSink>(std::move(sink)));
}

}  // namespace mmo::core
