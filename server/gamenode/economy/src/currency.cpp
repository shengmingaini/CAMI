// server/gamenode/economy/src/currency.cpp — TASK-029 §15.2

#include "mmo/game/economy/currency.h"

#include <utility>

namespace mmo::game::economy {

std::uint64_t WalletTable::Mix(std::uint64_t x) noexcept {
    // splitmix64 finalizer：把自增的 PlayerId 打散，避免低位聚集导致探测链变长。
    x += 0x9E3779B97F4A7C15ull;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    return x ^ (x >> 31);
}

void WalletTable::Rehash(std::size_t cap) {
    std::vector<PlayerId> new_keys(cap, 0);
    std::vector<std::uint32_t> new_vals(cap, 0);
    const std::size_t new_mask = cap - 1;
    for (std::size_t i = 0; i < owners_.size(); ++i) {
        std::size_t idx = static_cast<std::size_t>(Mix(owners_[i]) & new_mask);
        while (new_keys[idx] != 0) idx = (idx + 1) & new_mask;
        new_keys[idx] = owners_[i];
        new_vals[idx] = static_cast<std::uint32_t>(i);
    }
    keys_ = std::move(new_keys);
    vals_ = std::move(new_vals);
    mask_ = new_mask;
}

const Wallet* WalletTable::Find(PlayerId pid) const noexcept {
    if (pid == 0 || keys_.empty()) return nullptr;
    std::size_t idx = static_cast<std::size_t>(Mix(pid) & mask_);
    for (;;) {
        const PlayerId k = keys_[idx];
        if (k == 0) return nullptr;
        if (k == pid) return &wallets_[vals_[idx]];
        idx = (idx + 1) & mask_;
    }
}

Wallet* WalletTable::Find(PlayerId pid) noexcept {
    return const_cast<Wallet*>(static_cast<const WalletTable*>(this)->Find(pid));
}

Wallet& WalletTable::Fetch(PlayerId pid) noexcept {
    // 防御：PlayerId 0 是「未指定」，不得进表（否则会污染空槽哨兵）。
    static Wallet kOrphan{};
    if (pid == 0) return kOrphan;

    // 负载因子 0.7，超过则扩容一倍（保证探测链平均长度 < 2）。
    if ((used_ + 1) * 10 >= (mask_ + 1) * 7) Rehash((mask_ + 1) * 2);

    std::size_t idx = static_cast<std::size_t>(Mix(pid) & mask_);
    for (;;) {
        const PlayerId k = keys_[idx];
        if (k == 0) break;
        if (k == pid) return wallets_[vals_[idx]];
        idx = (idx + 1) & mask_;
    }
    keys_[idx] = pid;
    vals_[idx] = static_cast<std::uint32_t>(wallets_.size());
    owners_.push_back(pid);
    wallets_.emplace_back();
    ++used_;
    return wallets_.back();
}

}  // namespace mmo::game::economy
