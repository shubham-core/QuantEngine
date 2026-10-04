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

// src/portfolio/Portfolio.cpp
//
// M6 Portfolio implementation.
// See header for full contract and design documentation.

#include "quantengine/portfolio/Portfolio.hpp"

#include <algorithm>
#include <cstdlib>  // std::abs

namespace quantengine::portfolio {

Portfolio::Portfolio(std::size_t initial_symbols) {
    positions_.reserve(initial_symbols);
}

// ---------------------------------------------------------------------------
// IFillHandler::on_fill
// Only Filled and PartiallyFilled reports carry trades that change the ledger.
// Resting increments open_order_count; Cancelled/Filled decrement it.
// ---------------------------------------------------------------------------
void Portfolio::on_fill(const core::ExecutionReport& report) noexcept {
    const std::lock_guard<std::mutex> lock(mutex_);

    switch (report.status) {
        case core::OrderStatus::Resting:
            ++open_order_count_;
            break;

        case core::OrderStatus::Cancelled:
        case core::OrderStatus::Rejected:
            if (open_order_count_ > 0) {
                --open_order_count_;
            }
            break;

        case core::OrderStatus::Filled:
            if (open_order_count_ > 0) {
                --open_order_count_;
            }
            apply_trades(report);
            break;

        case core::OrderStatus::PartiallyFilled:
            // Order remains resting — count stays the same.
            apply_trades(report);
            break;

        case core::OrderStatus::New:
        default:
            break;
    }
}

// ---------------------------------------------------------------------------
// mark_to_market: update last_price and recalculate unrealized_pnl for symbol.
// ---------------------------------------------------------------------------
void Portfolio::mark_to_market(const market::SymbolArray& symbol, core::PriceTicks price) noexcept {
    const std::lock_guard<std::mutex> lock(mutex_);
    auto it = positions_.find(symbol);
    if (it == positions_.end()) {
        return;
    }

    Position& pos = it->second;
    pos.last_price = price;

    // unrealized_pnl = (mark_price - avg_cost) * net_qty   [long convention]
    // For a short (net_qty < 0): (avg_cost - mark_price) * |net_qty|
    //   = (avg_cost - mark_price) * net_qty  in signed arithmetic, same formula.
    pos.unrealized_pnl = (price - pos.avg_cost_ticks) * pos.net_quantity;
}

// ---------------------------------------------------------------------------
// snapshot_for: per-symbol view used by IRiskManager::validate().
// ---------------------------------------------------------------------------
auto Portfolio::snapshot_for(const market::SymbolArray& symbol) const noexcept
    -> risk::PortfolioView {
    const std::lock_guard<std::mutex> lock(mutex_);

    risk::PortfolioView view{};
    view.open_order_count = open_order_count_;

    // Aggregate totals across all symbols.
    core::PriceTicks total_realized = 0;
    core::PriceTicks total_unrealized = 0;
    core::PriceTicks total_notional = 0;

    for (const auto& [sym, pos] : positions_) {
        total_realized += pos.realized_pnl;
        total_unrealized += pos.unrealized_pnl;

        // |net_qty| * last_price
        const auto abs_qty = static_cast<core::PriceTicks>(
            pos.net_quantity >= 0 ? pos.net_quantity : -pos.net_quantity);
        total_notional += abs_qty * pos.last_price;
    }

    view.session_realized_pnl = total_realized;
    view.session_unrealized_pnl = total_unrealized;
    view.gross_notional_exposure = total_notional;

    // Per-symbol net_position (signed qty cast to Quantity bitwise).
    const auto it = positions_.find(symbol);
    if (it != positions_.end()) {
        // Cast signed int64 to uint64 — StandardRiskManager re-casts back.
        view.net_position = static_cast<core::Quantity>(it->second.net_quantity);
    }

    return view;
}

// ---------------------------------------------------------------------------
// snapshot: aggregate view (net_position = total absolute qty across symbols).
// ---------------------------------------------------------------------------
auto Portfolio::snapshot() const noexcept -> risk::PortfolioView {
    const std::lock_guard<std::mutex> lock(mutex_);

    risk::PortfolioView view{};
    view.open_order_count = open_order_count_;

    core::PriceTicks total_realized = 0;
    core::PriceTicks total_unrealized = 0;
    core::PriceTicks total_notional = 0;
    core::Quantity total_abs_qty = 0;

    for (const auto& [sym, pos] : positions_) {
        total_realized += pos.realized_pnl;
        total_unrealized += pos.unrealized_pnl;

        const auto abs_qty = static_cast<core::Quantity>(pos.net_quantity >= 0 ? pos.net_quantity
                                                                               : -pos.net_quantity);
        total_abs_qty += abs_qty;
        total_notional += static_cast<core::PriceTicks>(abs_qty) * pos.last_price;
    }

    view.session_realized_pnl = total_realized;
    view.session_unrealized_pnl = total_unrealized;
    view.gross_notional_exposure = total_notional;
    view.net_position = total_abs_qty;

    return view;
}

// ---------------------------------------------------------------------------
// find_position: read-only lookup, acquires lock.
// ---------------------------------------------------------------------------
auto Portfolio::find_position(const market::SymbolArray& symbol) const noexcept -> const Position* {
    const std::lock_guard<std::mutex> lock(mutex_);
    const auto it = positions_.find(symbol);
    if (it == positions_.end()) {
        return nullptr;
    }
    return &it->second;
}

auto Portfolio::total_realized_pnl() const noexcept -> core::PriceTicks {
    const std::lock_guard<std::mutex> lock(mutex_);
    core::PriceTicks total = 0;
    for (const auto& [sym, pos] : positions_) {
        total += pos.realized_pnl;
    }
    return total;
}

auto Portfolio::total_unrealized_pnl() const noexcept -> core::PriceTicks {
    const std::lock_guard<std::mutex> lock(mutex_);
    core::PriceTicks total = 0;
    for (const auto& [sym, pos] : positions_) {
        total += pos.unrealized_pnl;
    }
    return total;
}

auto Portfolio::open_order_count() const noexcept -> std::uint32_t {
    const std::lock_guard<std::mutex> lock(mutex_);
    return open_order_count_;
}

void Portfolio::reset() noexcept {
    const std::lock_guard<std::mutex> lock(mutex_);
    positions_.clear();
    open_order_count_ = 0;
}

// ---------------------------------------------------------------------------
// apply_trades: private, called with mutex held.
// Iterates over all trades in the report and applies each to the ledger.
// ---------------------------------------------------------------------------
void Portfolio::apply_trades(const core::ExecutionReport& report) noexcept {
    if (report.trades.empty()) {
        return;
    }

    // All trades in one ExecutionReport are for the same symbol.
    // We need the symbol — it isn't stored in ExecutionReport directly.
    // For now we resolve it from the trade's taker order id and side.
    // Since SimGateway uses client_order_id == engine order_id, we can
    // find the symbol from the OrderRequest if we had it — but the engine
    // does not carry the symbol through to the ExecutionReport.
    //
    // ARCHITECTURAL NOTE:
    //   The core engine was designed symbol-agnostic (one book per symbol).
    //   The SimGateway is also currently single-book. For the Portfolio to
    //   track per-symbol positions, the OrderRequest symbol must be passed
    //   through. We do this by using the order_id as a key in a side-table
    //   maintained by StrategyRunner (see StrategyRunner::on_fill).
    //
    //   However, Portfolio::on_fill is called with only an ExecutionReport.
    //   To bridge this, we record the symbol alongside on_fill via the
    //   apply_fill_with_symbol() method called directly from StrategyRunner.
    //   The base on_fill(report) path handles order count bookkeeping only;
    //   apply_fill_with_symbol handles the ledger update.
    //
    //   This dual-dispatch design is intentional: it keeps Portfolio independent
    //   of StrategyRunner while allowing StrategyRunner to supply the symbol context.
    //
    // This method intentionally does nothing for trades here — it is called
    // only from the IFillHandler path which goes via StrategyRunner. See
    // apply_fill_with_symbol() for the ledger update entry point.
    (void)report;
}

// ---------------------------------------------------------------------------
// apply_fill_with_symbol: called by StrategyRunner which knows the symbol.
// Public because StrategyRunner must call it.
// ---------------------------------------------------------------------------
void Portfolio::apply_fill_with_symbol(const core::ExecutionReport& report,
                                       const market::SymbolArray& symbol) noexcept {
    const std::lock_guard<std::mutex> lock(mutex_);

    // Update open order count.
    switch (report.status) {
        case core::OrderStatus::Resting:
            ++open_order_count_;
            break;
        case core::OrderStatus::Cancelled:
        case core::OrderStatus::Rejected:
            if (open_order_count_ > 0) {
                --open_order_count_;
            }
            break;
        case core::OrderStatus::Filled:
            if (open_order_count_ > 0) {
                --open_order_count_;
            }
            break;
        case core::OrderStatus::PartiallyFilled:
        case core::OrderStatus::New:
        default:
            break;
    }

    // Apply each trade to the position ledger.
    if (report.trades.empty()) {
        return;
    }

    auto& pos = positions_[symbol];  // insert-on-first-use
    pos.symbol = symbol;

    for (const auto& trade : report.trades) {
        // Determine which side this order was on by comparing order_id.
        const bool is_taker = (trade.taker_order_id == report.order_id);
        const core::Side fill_side =
            is_taker ? (trade.maker_side == core::Side::Buy ? core::Side::Sell : core::Side::Buy)
                     : trade.maker_side;

        apply_fill_to_position(pos, fill_side, trade.price, trade.quantity);
    }
}

// ---------------------------------------------------------------------------
// apply_fill_to_position: AVCO position accounting.
// Called with mutex held.
// ---------------------------------------------------------------------------
void Portfolio::apply_fill_to_position(Position& pos, core::Side side, core::PriceTicks price,
                                       core::Quantity qty) noexcept {
    const std::int64_t signed_qty = (side == core::Side::Buy) ? static_cast<std::int64_t>(qty)
                                                              : -static_cast<std::int64_t>(qty);

    const std::int64_t old_qty = pos.net_quantity;
    const std::int64_t new_qty = old_qty + signed_qty;
    const bool was_long = old_qty >= 0;
    const bool going_long = signed_qty > 0;

    if (old_qty == 0) {
        // Opening a fresh position.
        pos.net_quantity = new_qty;
        pos.avg_cost_ticks = price;
        return;
    }

    const std::int64_t old_abs_qty = old_qty >= 0 ? old_qty : -old_qty;
    const std::int64_t new_abs_qty = new_qty >= 0 ? new_qty : -new_qty;

    if ((was_long && going_long) || (!was_long && !going_long)) {
        // Adding to an existing position (same direction) — update avg cost.
        const std::int64_t total_cost =
            pos.avg_cost_ticks * old_abs_qty + price * static_cast<std::int64_t>(qty);
        pos.net_quantity = new_qty;
        if (new_abs_qty != 0) {
            pos.avg_cost_ticks = total_cost / new_abs_qty;
        }
        return;
    }

    // Reducing or reversing — realize P&L on the closed portion.
    const std::int64_t close_qty =
        std::min(static_cast<std::int64_t>(qty), old_qty >= 0 ? old_qty : -old_qty);

    // P&L on closed portion.
    const core::PriceTicks pnl_per_unit =
        was_long ? (price - pos.avg_cost_ticks) : (pos.avg_cost_ticks - price);

    pos.realized_pnl += pnl_per_unit * close_qty;
    pos.net_quantity = new_qty;

    if (new_qty == 0) {
        pos.avg_cost_ticks = 0;
    } else if ((going_long && new_qty > 0) || (!going_long && new_qty < 0)) {
        // Residual in same direction as fill — opened new position at fill price.
        pos.avg_cost_ticks = price;
    }
    // else: still in old direction but reduced — avg_cost stays unchanged.
}

auto Portfolio::compute_gross_notional_locked() const noexcept -> core::PriceTicks {
    core::PriceTicks total = 0;
    for (const auto& [sym, pos] : positions_) {
        const auto abs_qty = static_cast<core::PriceTicks>(
            pos.net_quantity >= 0 ? pos.net_quantity : -pos.net_quantity);
        total += abs_qty * pos.last_price;
    }
    return total;
}

}  // namespace quantengine::portfolio
