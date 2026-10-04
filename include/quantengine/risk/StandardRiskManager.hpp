// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2024 QuantEngine Authors
//
// This file is part of QuantEngine.
// QuantEngine is free software: you can redistribute it and/or modify it under
// the terms of the GNU Affero General Public License as published by the Free
// Software Foundation, either version 3 of the License, or (at your option)
// any later version.
//
// For commercial licensing without the AGPL restrictions, contact:
//   commercial@quantengine.io

#pragma once

// quantengine/risk/StandardRiskManager.hpp
//
// Concrete pre-trade risk implementation satisfying IRiskManager.
// Implementation is in src/risk/StandardRiskManager.cpp.
//
// LIMIT HIERARCHY (evaluated in strict order; first breach wins):
//   1. max_order_quantity   — per-order size cap (exclusive upper bound)
//   2. max_order_notional   — per-order price*qty cap (exclusive upper bound)
//   3. max_position_quantity — |position + order_qty| cap (exclusive upper bound)
//   4. max_gross_exposure   — portfolio gross notional cap (exclusive upper bound)
//   5. daily_drawdown_limit — halt when session_pnl() <= -limit (sticky)
//
// Zero-value limits are treated as DISABLED (pass through). This matches
// RiskLimits default-construction semantics.
//
// THREAD SAFETY:
//   validate()       — safe to call concurrently from the strategy thread
//   update_limits()  — safe to call concurrently from the config thread
//   is_halted()      — lock-free atomic read
//   reset_halt()     — lock-free atomic write
//
// ALLOCATION:
//   validate() is zero-allocation on the hot path after construction.
//   The client_order_id tracking set pre-reserves capacity at construction.
//   The set grows on first-time order IDs (amortised O(1)). For strictly
//   zero-alloc duplicate detection, replace with a Bloom filter (P2 scope).

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string_view>
#include <unordered_set>

#include "quantengine/risk/IRiskManager.hpp"

namespace quantengine::risk {

class StandardRiskManager final : public IRiskManager {
public:
    // initial_capacity: pre-reserved slots in the order-id tracking set.
    // Tune to expected order count per session to avoid rehash on hot path.
    explicit StandardRiskManager(RiskLimits limits = RiskLimits::no_limit(),
                                 std::size_t initial_capacity = 4096);

    ~StandardRiskManager() override = default;

    StandardRiskManager(const StandardRiskManager&) = delete;
    auto operator=(const StandardRiskManager&) -> StandardRiskManager& = delete;
    StandardRiskManager(StandardRiskManager&&) = delete;
    auto operator=(StandardRiskManager&&) -> StandardRiskManager& = delete;

    // Core hot-path method — O(1) amortised, noexcept, zero-allocation after warm-up.
    [[nodiscard]] auto validate(const execution::OrderRequest& request,
                                const PortfolioView& portfolio) const noexcept
        -> RiskVerdict override;

    // Atomically swaps the active limit set. Safe to call while validate() runs.
    void update_limits(const RiskLimits& limits) noexcept override;

    [[nodiscard]] auto current_limits() const noexcept -> RiskLimits override;

    // Returns true when the drawdown halt is engaged. Lock-free.
    [[nodiscard]] auto is_halted() const noexcept -> bool override;

    // Clears the halt. Must be called explicitly — never auto-clears.
    void reset_halt() noexcept override;

    [[nodiscard]] auto name() const noexcept -> std::string_view override {
        return "StandardRiskManager";
    }

private:
    // validate() is declared const (required by interface) but must write
    // two pieces of mutable state: the halt flag and the seen-id set.
    // Both are declared mutable and access is synchronised appropriately.

    // Limits: protected by mutex_ (updated rarely, read on every validate).
    mutable std::mutex limits_mutex_;
    RiskLimits limits_;

    // Halt flag: atomic so is_halted() and reset_halt() need no mutex.
    mutable std::atomic<bool> halted_{false};

    // Client-order-id deduplication set.
    // Mutable because validate() writes to it on the first approval of each id.
    // Protected by seen_ids_mutex_ to allow concurrent validate() calls.
    // Note: concurrent writes from multiple strategy threads are rare in practice;
    // the mutex is uncontended on the happy path.
    mutable std::mutex seen_ids_mutex_;
    mutable std::unordered_set<core::OrderId> seen_ids_;
};

}  // namespace quantengine::risk
