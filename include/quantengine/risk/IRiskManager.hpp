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

// quantengine/risk/IRiskManager.hpp
//
// Pre-trade risk interface — validates an OrderRequest before it reaches the
// execution gateway. Returns a structured approval or rejection.
//
// DESIGN RATIONALE:
//   Pre-trade risk is synchronous and on the hot path. Every submit_order()
//   call passes through the risk manager before touching the gateway. This
//   means the risk check MUST be:
//     - noexcept (a throw aborts the order loop)
//     - zero-allocation (no heap touches on the decision path)
//     - O(1) in the common (approved) case
//
//   The risk manager is stateful: it tracks running position, notional
//   exposure, and session P&L by reading from a Portfolio read-view.
//   The Portfolio is written by the IFillHandler on every fill.
//
//   DEPENDENCY: IRiskManager reads Portfolio. Portfolio is written by fills.
//   This creates a read-after-write dependency on the fill path. The
//   implementation must ensure fill application is atomic from the risk
//   manager's perspective (a single mutex or a lock-free snapshot suffices).
//
// LIMITS HIERARCHY (applied in this order; first breach wins):
//   1. max_order_quantity    — per-order size cap (absolute)
//   2. max_order_notional    — per-order notional cap (price * qty)
//   3. max_position_quantity — per-symbol position cap (long + short)
//   4. max_gross_exposure    — total portfolio notional cap
//   5. daily_drawdown_halt   — session P&L floor (triggers trading halt)
//
// THREAD SAFETY:
//   validate() is called from the strategy thread.
//   update_limits() is called from the main/config thread.
//   Implementations must synchronise access to limit parameters.
//   Reading Portfolio state must be thread-safe relative to fill application.
//
// LIFETIME:
//   IRiskManager holds a reference to a PortfolioView (read-only portfolio
//   snapshot). The Portfolio object must outlive the risk manager.

#include <cstdint>
#include <limits>
#include <string_view>

#include "quantengine/core/types.hpp"
#include "quantengine/execution/IExecutionGateway.hpp"
#include "quantengine/market/MarketEvent.hpp"

namespace quantengine::risk {

// ---------------------------------------------------------------------------
// RiskRejectReason: structured rejection cause, returned alongside the
// RiskVerdict. Distinct from core::RejectReason (which is engine-internal).
// ---------------------------------------------------------------------------
enum class RiskRejectReason : std::uint8_t {
    None = 0,               // not rejected
    OrderTooLarge,          // qty > max_order_quantity
    NotionalTooLarge,       // price * qty > max_order_notional
    PositionLimitBreached,  // adding this order would exceed position cap
    ExposureLimitBreached,  // would exceed gross portfolio notional cap
    DrawdownHaltActive,     // daily drawdown limit triggered; trading halted
    DuplicateClientId,      // client_order_id reused within this session
    InvalidSymbol,          // symbol not in the approved instrument universe
    GatewayNotConnected,    // risk manager requires an active gateway
    InternalError
};

[[nodiscard]] constexpr auto to_string(RiskRejectReason r) noexcept -> std::string_view {
    switch (r) {
        case RiskRejectReason::None:
            return "NONE";
        case RiskRejectReason::OrderTooLarge:
            return "ORDER_TOO_LARGE";
        case RiskRejectReason::NotionalTooLarge:
            return "NOTIONAL_TOO_LARGE";
        case RiskRejectReason::PositionLimitBreached:
            return "POSITION_LIMIT_BREACHED";
        case RiskRejectReason::ExposureLimitBreached:
            return "EXPOSURE_LIMIT_BREACHED";
        case RiskRejectReason::DrawdownHaltActive:
            return "DRAWDOWN_HALT_ACTIVE";
        case RiskRejectReason::DuplicateClientId:
            return "DUPLICATE_CLIENT_ID";
        case RiskRejectReason::InvalidSymbol:
            return "INVALID_SYMBOL";
        case RiskRejectReason::GatewayNotConnected:
            return "GATEWAY_NOT_CONNECTED";
        case RiskRejectReason::InternalError:
            return "INTERNAL_ERROR";
    }
    return "UNKNOWN";
}

// ---------------------------------------------------------------------------
// RiskVerdict: the output of IRiskManager::validate().
// 8 bytes — passed by value; zero heap.
//
// If approved == false, the gateway MUST NOT receive the order.
// The reject_reason gives the strategy a machine-readable cause.
// ---------------------------------------------------------------------------
struct RiskVerdict {
    bool approved{false};
    RiskRejectReason reject_reason{RiskRejectReason::InternalError};
    std::uint8_t pad[6]{};

    [[nodiscard]] constexpr auto operator==(const RiskVerdict&) const noexcept -> bool = default;

    [[nodiscard]] static constexpr auto accept() noexcept -> RiskVerdict {
        return RiskVerdict{.approved = true, .reject_reason = RiskRejectReason::None};
    }

    [[nodiscard]] static constexpr auto reject(RiskRejectReason reason) noexcept -> RiskVerdict {
        return RiskVerdict{.approved = false, .reject_reason = reason};
    }
};
static_assert(sizeof(RiskVerdict) == 8, "RiskVerdict layout changed — update docs");

// ---------------------------------------------------------------------------
// RiskLimits: the configurable limit set for one trading session.
// All fields use the same fixed-point types as the engine (PriceTicks, Qty).
// Notional values are in ticks * quantity — the implementation must apply
// the instrument's tick size to convert to USD if needed.
//
// Set a limit to its max value to disable it (no_limit pattern).
// ---------------------------------------------------------------------------
struct RiskLimits {
    core::Quantity max_order_quantity{0};      // 0 = disabled (use no_limit())
    core::Quantity max_position_quantity{0};   // per symbol, long + short
    core::PriceTicks max_order_notional{0};    // price_ticks * qty cap
    core::PriceTicks max_gross_exposure{0};    // total portfolio notional cap
    core::PriceTicks daily_drawdown_limit{0};  // halt when session_pnl <= -limit

    // Convenience: returns limits with all checks disabled.
    // Intended for testing only — never use in production.
    [[nodiscard]] static constexpr auto no_limit() noexcept -> RiskLimits {
        using L = std::numeric_limits<core::Quantity>;
        using P = std::numeric_limits<core::PriceTicks>;
        return RiskLimits{.max_order_quantity = L::max(),
                          .max_position_quantity = L::max(),
                          .max_order_notional = P::max(),
                          .max_gross_exposure = P::max(),
                          .daily_drawdown_limit = P::max()};
    }

    [[nodiscard]] constexpr auto operator==(const RiskLimits&) const noexcept -> bool = default;
};

// ---------------------------------------------------------------------------
// PortfolioView: a read-only snapshot of portfolio state consumed by the
// risk manager. Deliberately minimal — only what risk checks need.
//
// The Portfolio implementation writes to this state; the risk manager reads.
// Both must agree on the memory model (atomic reads or mutex snapshot).
//
// This is a struct of plain integers — no heap, no virtual dispatch.
// The full Portfolio (with P&L history, position ledger) lives in its own
// class (M6). The risk manager only needs this subset.
// ---------------------------------------------------------------------------
struct PortfolioView {
    core::PriceTicks session_realized_pnl{0};     // cumulative fills this session
    core::PriceTicks session_unrealized_pnl{0};   // mark-to-market since last quote
    core::PriceTicks gross_notional_exposure{0};  // sum of |position| * last_price
    core::Quantity net_position{0};               // signed: positive=long, cast as needed
    std::uint32_t open_order_count{0};
    std::uint32_t pad{0};

    [[nodiscard]] constexpr auto session_pnl() const noexcept -> core::PriceTicks {
        return session_realized_pnl + session_unrealized_pnl;
    }

    [[nodiscard]] constexpr auto operator==(const PortfolioView&) const noexcept -> bool = default;
};
static_assert(sizeof(PortfolioView) == 40, "PortfolioView layout changed — update docs");

// ---------------------------------------------------------------------------
// IRiskManager: abstract pre-trade risk interface.
//
// Implementations:
//   StandardRiskManager — checks the five-limit hierarchy (M7 scope)
//   NullRiskManager     — approves all orders (testing only)
//
// CALL ORDER (enforced by strategy wiring in M8):
//   1. Caller assigns client_order_id to request
//   2. risk.validate(request, portfolio_snapshot) -> RiskVerdict
//   3. If approved: gateway.submit_order(request) -> OrderAck
//   4. If rejected: log, do not submit
// ---------------------------------------------------------------------------
class IRiskManager {
public:
    IRiskManager() = default;
    virtual ~IRiskManager() = default;

    IRiskManager(const IRiskManager&) = delete;
    auto operator=(const IRiskManager&) -> IRiskManager& = delete;
    IRiskManager(IRiskManager&&) = default;
    auto operator=(IRiskManager&&) -> IRiskManager& = default;

    // Core hot-path method. Called synchronously before every order submission.
    // MUST be noexcept. MUST NOT allocate. MUST complete in O(1).
    // portfolio: a snapshot of current portfolio state for limit evaluation.
    [[nodiscard]] virtual auto validate(const execution::OrderRequest& request,
                                        const PortfolioView& portfolio) const noexcept
        -> RiskVerdict = 0;

    // Update the limit set. Thread-safe with validate() — implementation
    // must use appropriate synchronisation (e.g. atomic swap of a limits pod).
    // NOT called on the hot path.
    virtual void update_limits(const RiskLimits& limits) noexcept = 0;

    // Read the current active limits (for monitoring / audit logging).
    [[nodiscard]] virtual auto current_limits() const noexcept -> RiskLimits = 0;

    // Returns true if trading is currently halted (drawdown limit breached).
    // Once halted, only the operations team / explicit reset can re-enable.
    [[nodiscard]] virtual auto is_halted() const noexcept -> bool = 0;

    // Reset the halt state. Must be called explicitly — never auto-resets.
    // Callers must verify root cause before calling this.
    virtual void reset_halt() noexcept = 0;

    // Human-readable identifier for logging.
    [[nodiscard]] virtual auto name() const noexcept -> std::string_view = 0;
};

// ---------------------------------------------------------------------------
// NullRiskManager: approves all orders, never halts.
// Use ONLY in unit tests to isolate strategy logic from risk checks.
// Instantiating this in production is a design error.
// ---------------------------------------------------------------------------
class NullRiskManager final : public IRiskManager {
public:
    [[nodiscard]] auto validate(const execution::OrderRequest& /*request*/,
                                const PortfolioView& /*portfolio*/) const noexcept
        -> RiskVerdict override {
        return RiskVerdict::accept();
    }

    void update_limits(const RiskLimits& /*limits*/) noexcept override {}

    [[nodiscard]] auto current_limits() const noexcept -> RiskLimits override {
        return RiskLimits::no_limit();
    }

    [[nodiscard]] auto is_halted() const noexcept -> bool override { return false; }

    void reset_halt() noexcept override {}

    [[nodiscard]] auto name() const noexcept -> std::string_view override {
        return "NullRiskManager";
    }
};

}  // namespace quantengine::risk
