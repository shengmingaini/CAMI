// server/dataservice/include/mmo/data/fake_data_store.h
//
// FakeDataStore —— 可注入故障的 IDataStore 测试替身（TASK-026 §15.10）。
//
// 用于验证重试与冲突路径，无需真实外部存储：可注入
//   - 延迟（模拟慢请求）
//   - 错误（模拟存储不可用，Load 返回 INTERNAL_ERROR）
//   - 版本冲突（模拟并发写入者，Save/Delete 返回 VERSION_CONFLICT）
//
// 头文件实现，仅用于测试；不进入生产链接路径（业务实现由后续任务提供）。

#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <span>
#include <thread>
#include <unordered_map>
#include <vector>

#include "mmo/core/error/error_code.h"
#include "mmo/data/idata_store.h"
#include "mmo/data/record.h"
#include "mmo/data/result_helpers.h"

namespace mmo::data {

class FakeDataStore final : public IDataStore {
public:
    void set_injected_delay(mmo::core::DurationMs d) noexcept { delay_ = d; }
    void set_fail_loads(bool on) noexcept { fail_loads_ = on; }
    void set_force_conflict(bool on) noexcept { force_conflict_ = on; }

    std::uint64_t load_calls() const noexcept { return load_calls_; }
    std::uint64_t save_calls() const noexcept { return save_calls_; }

    core::Result<std::optional<Record>> Load(const DataKey& key) override {
        ++load_calls_;
        sleep();
        if (fail_loads_) return fail<std::optional<Record>>(core::ErrorCode::INTERNAL_ERROR, "store unavailable");
        auto it = map_.find(key);
        if (it == map_.end()) return core::Result<std::optional<Record>>::Ok(std::nullopt);
        return core::Result<std::optional<Record>>::Ok(it->second);
    }

    core::Result<void> Save(const Record& rec, VersionCheck vc = {}) override {
        ++save_calls_;
        sleep();
        if (force_conflict_) {
            return fail<void>(core::ErrorCode::VERSION_CONFLICT, "injected conflict");
        }
        map_[rec.key] = rec;
        return core::Result<void>::Ok();
    }

    core::Result<void> Delete(const DataKey& key, VersionCheck vc = {}) override {
        sleep();
        if (force_conflict_) {
            return fail<void>(core::ErrorCode::VERSION_CONFLICT, "injected conflict");
        }
        map_.erase(key);
        return core::Result<void>::Ok();
    }

    core::Result<std::vector<Record>> BatchLoad(std::span<const DataKey> keys) override {
        std::vector<Record> out;
        for (const auto& k : keys) {
            auto it = map_.find(k);
            if (it != map_.end()) out.push_back(it->second);
        }
        return core::Result<std::vector<Record>>::Ok(std::move(out));
    }

    core::Result<std::vector<BatchOutcome>> BatchSave(std::span<const Record> recs) override {
        std::vector<BatchOutcome> out;
        out.reserve(recs.size());
        for (const auto& r : recs) {
            BatchOutcome o;
            o.key = r.key;
            if (force_conflict_) {
                o.ok = false;
                o.code = static_cast<std::uint32_t>(core::ErrorCode::VERSION_CONFLICT);
            } else {
                map_[r.key] = r;
                o.ok = true;
                o.code = 0;
            }
            out.push_back(std::move(o));
        }
        return core::Result<std::vector<BatchOutcome>>::Ok(std::move(out));
    }

private:
    void sleep() const {
        if (delay_ > mmo::core::DurationMs{0}) std::this_thread::sleep_for(delay_);
    }

    std::unordered_map<DataKey, Record> map_;
    mmo::core::DurationMs delay_{};
    bool fail_loads_{false};
    bool force_conflict_{false};
    std::uint64_t load_calls_{0};
    std::uint64_t save_calls_{0};
};

}  // namespace mmo::data
