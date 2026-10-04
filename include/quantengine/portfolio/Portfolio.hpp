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

// quantengine/portfolio/Portfolio.hpp
//
// M6: Session portfolio tracker. Records per-symbol positions, realized P&L,
// unrealized P&L (mark-to-market), and gross notional exposure.
//
// ARCHITECTURE:
//   Portfolio implements IFillHandler, so SimGateway can call on_fill()
//   directly. StrategyRunner (M8) fans out on_fill() to both Portfolio and
//   IStrategy::on_fill().
//
//   A per-symbol PortfolioView is supplied to IRiskManager::validate() before
//   each order. The view carries:
//     - net_position          = signed qty for that symbol
//     - gross_notional_exposure = total portfolio (all symbols) for exposure limit
//     - session_realized_pnl  = total session realized P&L (for drawdown check)
//     - session_unrealized_pnl = total mark-to-market unrealized P&L
//
// POSITION ACCOUNTING:
//   Average cost (FIFO by default; AVCO chosen here for simplicity):
//     When adding to a position:
//       new_avg_cost = (old_qty * old_avg + fill_qty * fill_price) / new_qty
//     When reducing/closing:
//       realize = (fill_price - avg_cost) * closed_qty  [for longs]
//       realize = (avg_cost - fill_price) * closed_qty  [for shorts]
//     When reversing:
//       close existing at old avg_cost, open new in opposite direction.
//
// THREAD SAFETY:
//   on_fill()         — may be called from gateway thread (SimGateway: same thread)
//   mark_to_market()  — called from strategy/market-data thread
//   snapshot_for()    — called from risk thread (strategy thread in practice)
//   All public methods acquire mutex_. For SimGateway (single-threaded) the
//   mutex is uncontended and costs nothing.
//
// ALLOCATION:
//   The symbol→position map (std::unordered_map) allocates on first insert
//   per symbol. Pre-reserve with expected symbol count to avoid rehash.
//   After warm-up the hot path (on_fill for known symbol) is zero-alloc.

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string_view>
#include <unordered_map>

#include "quantengine/core/events.hpp"
#include "quantengine/core/types.hpp"
#include "quantengine/execution/IExecutionGateway.hpp"
#include "quantengine/market/MarketEvent.hpp"
#include "quantengine/risk/IRiskManager.hpp"

namespace quantengine::portfolio {

// ---------------------------------------------------------------------------
// Position: per-symbol ledger entry.
// avg_cost_ticks: weighted-average cost basis in raw PriceTicks (same units
//   as engine prices). Multiply by tick_size to get USD cost per share.
// ---------------------------------------------------------------------------
struct Position {
    market::SymbolArray symbol{};
    std::int64_t net_quantity{0};        // +long / -short
    core::PriceTicks avg_cost_ticks{0};  // weighted avg cost in engine ticks
    core::PriceTicks realized_pnl{0};    // cumulative realized for this symbol
    core::PriceTicks unrealized_pnl{0};  // mark-to-market using last quote
    core::PriceTicks last_price{0};      // most recent mark price

    [[nodiscard]] auto operator==(const Position&) const noexcept -> bool = default;
};

// ---------------------------------------------------------------------------
// Portfolio: session-scoped position and P&L ledger.
// ---------------------------------------------------------------------------
class Portfolio final : public execution::IFillHandler {
public:
    // initial_symbols: pre-reserve capacity in the position map.
    explicit Portfolio(std::size_t initial_symbols = 32);

    ~Portfolio() override = default;

    Portfolio(const Portfolio&) = delete;
    auto operator=(const Portfolio&) -> Portfolio& = delete;
    Portfolio(Portfolio&&) = delete;
    auto operator=(Portfolio&&) -> Portfolio& = delete;

    // ---- IFillHandler -------------------------------------------------------

    // Process an ExecutionReport. Only Filled and PartiallyFilled statuses
    // (i.e. reports that contain at least one Trade) update the ledger.
    // Resting / Cancelled / Rejected reports update open_order_count only.
    void on_fill(const core::ExecutionReport& report) noexcept override;

    // Called by StrategyRunner which has the symbol context the engine lacks.
    // This is the primary ledger-update entry point. Prefer this over on_fill()
    // when the symbol is known (i.e., always from StrategyRunner).
    void apply_fill_with_symbol(const core::ExecutionReport& report,
                                const market::SymbolArray& symbol) noexcept;

    void on_gateway_connected() noexcept override {}
    void on_gateway_disconnected(std::string_view /*reason*/) noexcept override {}

    // ---- Mark-to-market -----------------------------------------------------

    // Update the mark price for a symbol. Recalculates unrealized_pnl for
    // that symbol's position. Called by StrategyRunner on every Quote tick.
    void mark_to_market(const market::SymbolArray& symbol, core::PriceTicks price) noexcept;

    // ---- Snapshots for risk -------------------------------------------------

    // Returns a PortfolioView for a specific symbol.
    // net_position  = signed qty for that symbol (as Quantity via reinterpret).
    // gross_notional_exposure = TOTAL portfolio gross notional (all symbols).
    // session_realized_pnl / unrealized_pnl = TOTAL session (all symbols).
    [[nodiscard]] auto snapshot_for(const market::SymbolArray& symbol) const noexcept
        -> risk::PortfolioView;

    // Returns the total session snapshot (net_position = aggregate |qty| sum).
    [[nodiscard]] auto snapshot() const noexcept -> risk::PortfolioView;

    // ---- Inspection ---------------------------------------------------------

    // Find the position record for a symbol. Returns nullptr if not found.
    [[nodiscard]] auto find_position(const market::SymbolArray& symbol) const noexcept
        -> const Position*;

    // Aggregate realized P&L across all symbols.
    [[nodiscard]] auto total_realized_pnl() const noexcept -> core::PriceTicks;

    // Aggregate unrealized P&L across all symbols.
    [[nodiscard]] auto total_unrealized_pnl() const noexcept -> core::PriceTicks;

    // Count of currently resting (unmatched) orders the portfolio is tracking.
    [[nodiscard]] auto open_order_count() const noexcept -> std::uint32_t;

    // Reset all state (for test isolation between sessions).
    void reset() noexcept;

private:
    // Apply a set of Trades from an ExecutionReport to the ledger.
    void apply_trades(const core::ExecutionReport& report) noexcept;

    // Apply a single fill to one position record.
    void apply_fill_to_position(Position& pos, core::Side side, core::PriceTicks price,
                                core::Quantity qty) noexcept;

    // Compute gross notional exposure (must be called with mutex held).
    [[nodiscard]] auto compute_gross_notional_locked() const noexcept -> core::PriceTicks;

    mutable std::mutex mutex_;
    std::unordered_map<market::SymbolArray, Position, market::SymbolHash> positions_;
    std::uint32_t open_order_count_{0};
};

}  // namespace quantengine::portfolio
