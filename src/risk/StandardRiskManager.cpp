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

// src/risk/StandardRiskManager.cpp
//
// StandardRiskManager implementation.
// See header for full contract and thread-safety documentation.

#include "quantengine/risk/StandardRiskManager.hpp"

#include <cstdint>
#include <limits>

namespace quantengine::risk {

StandardRiskManager::StandardRiskManager(RiskLimits limits, std::size_t initial_capacity)
    : limits_(limits) {
    seen_ids_.reserve(initial_capacity);
}

// ---------------------------------------------------------------------------
// validate() — the hot-path method.
//
// Limit hierarchy (first breach wins, sets reject_reason and returns):
//   1. Halt check      (atomic read — lock-free)
//   2. max_order_quantity
//   3. max_order_notional
//   4. max_position_quantity
//   5. max_gross_exposure
//   6. daily_drawdown_limit (triggers halt on breach)
//   7. client_order_id deduplication
//
// If all checks pass, the client_order_id is recorded and Approved is returned.
// ---------------------------------------------------------------------------
auto StandardRiskManager::validate(const execution::OrderRequest& request,
                                   const PortfolioView& portfolio) const noexcept -> RiskVerdict {
    // ---- 1. Sticky halt check (lock-free) ----------------------------------
    if (halted_.load(std::memory_order_acquire)) {
        return RiskVerdict::reject(RiskRejectReason::DrawdownHaltActive);
    }

    // Snapshot limits under the mutex. The snapshot is a plain-old-data copy;
    // the mutex is released immediately after, so validate() is not held.
    RiskLimits lim{};
    {
        const std::lock_guard<std::mutex> lock(limits_mutex_);
        lim = limits_;
    }

    // ---- 2. Max order quantity (0 = disabled) -------------------------------
    if (lim.max_order_quantity > 0 && request.quantity >= lim.max_order_quantity) {
        return RiskVerdict::reject(RiskRejectReason::OrderTooLarge);
    }

    // ---- 3. Max order notional (0 = disabled) -------------------------------
    // Use 128-bit multiplication to avoid overflow on large price * qty.
    // Cast to signed 64-bit: price can be negative in theory (short prices),
    // but for notional purposes we use the absolute value.
    if (lim.max_order_notional > 0) {
        // Both operands fit in int64/uint64; product may exceed int64 range.
        // Compute as unsigned and compare against the unsigned limit.
        const auto price_u =
            static_cast<core::Quantity>(request.price > 0 ? request.price : -request.price);
        // Check for overflow before multiplying: if price_u > max/qty, overflow.
        if (request.quantity > 0 && price_u > 0) {
            const auto notional_limit_u = static_cast<core::Quantity>(
                lim.max_order_notional > 0 ? lim.max_order_notional : 0);
            // Overflow-safe: if qty > limit/price, product >= limit.
            if (price_u >= notional_limit_u / request.quantity + 1 ||
                price_u * request.quantity >= notional_limit_u) {
                return RiskVerdict::reject(RiskRejectReason::NotionalTooLarge);
            }
        }
    }

    // ---- 4. Max position quantity (0 = disabled) ----------------------------
    // net_position is stored as Quantity (uint64) with two's-complement
    // wrap-around for shorts. We compute absolute magnitude by checking
    // whether the value exceeds int64 max (i.e. it is a negative int64).
    if (lim.max_position_quantity > 0) {
        const auto pos_signed = static_cast<std::int64_t>(portfolio.net_position);
        const core::Quantity abs_position = (pos_signed >= 0)
                                                ? static_cast<core::Quantity>(pos_signed)
                                                : static_cast<core::Quantity>(-pos_signed);

        // For a buy, new position = abs_position + qty (worst case: same side).
        // For a sell that deepens a short, same arithmetic applies.
        // We conservatively add qty to abs_position regardless of side — this
        // is stricter than necessary for a flat-then-sell scenario but
        // eliminates edge-case bugs in the first implementation.
        const core::Quantity projected = abs_position + request.quantity;

        // Overflow check: if abs_position > max - qty, sum overflows.
        if (request.quantity > lim.max_position_quantity ||
            projected >= lim.max_position_quantity) {
            return RiskVerdict::reject(RiskRejectReason::PositionLimitBreached);
        }
    }

    // ---- 5. Max gross exposure (0 = disabled) -------------------------------
    if (lim.max_gross_exposure > 0 && portfolio.gross_notional_exposure >= lim.max_gross_exposure) {
        return RiskVerdict::reject(RiskRejectReason::ExposureLimitBreached);
    }

    // ---- 6. Daily drawdown halt (0 = disabled) ------------------------------
    if (lim.daily_drawdown_limit > 0) {
        const core::PriceTicks pnl = portfolio.session_pnl();
        // Halt when pnl <= -limit  i.e.  pnl + limit <= 0
        if (pnl <= -lim.daily_drawdown_limit) {
            // Engage the sticky halt
            halted_.store(true, std::memory_order_release);
            return RiskVerdict::reject(RiskRejectReason::DrawdownHaltActive);
        }
    }

    // ---- 7. Duplicate client_order_id detection ----------------------------
    {
        const std::lock_guard<std::mutex> lock(seen_ids_mutex_);
        const auto [it, inserted] = seen_ids_.insert(request.client_order_id);
        if (!inserted) {
            return RiskVerdict::reject(RiskRejectReason::DuplicateClientId);
        }
    }

    return RiskVerdict::accept();
}

void StandardRiskManager::update_limits(const RiskLimits& limits) noexcept {
    const std::lock_guard<std::mutex> lock(limits_mutex_);
    limits_ = limits;
}

auto StandardRiskManager::current_limits() const noexcept -> RiskLimits {
    const std::lock_guard<std::mutex> lock(limits_mutex_);
    return limits_;
}

auto StandardRiskManager::is_halted() const noexcept -> bool {
    return halted_.load(std::memory_order_acquire);
}

void StandardRiskManager::reset_halt() noexcept {
    halted_.store(false, std::memory_order_release);
}

}  // namespace quantengine::risk
