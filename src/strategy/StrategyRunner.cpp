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

// src/strategy/StrategyRunner.cpp
//
// M8 StrategyRunner implementation.
// See header for full contract and architecture documentation.

#include "quantengine/strategy/StrategyRunner.hpp"

#include <variant>

namespace quantengine::strategy {

StrategyRunner::StrategyRunner(market::IMarketDataFeed& feed, risk::IRiskManager& risk,
                               execution::IExecutionGateway& gateway,
                               portfolio::Portfolio& portfolio, IStrategy& strategy)
    : feed_(feed), risk_(risk), gateway_(gateway), portfolio_(portfolio), strategy_(strategy) {
    pending_orders_.reserve(512);
}

StrategyRunner::~StrategyRunner() {
    if (running_) {
        stop();
    }
}

// ---------------------------------------------------------------------------
// run(): connect → subscribe → on_start → start feed.
// For SimGateway + synchronous feed: returns after all events dispatched.
// ---------------------------------------------------------------------------
auto StrategyRunner::run() noexcept -> bool {
    if (running_) {
        return false;
    }

    // Connect gateway — registers StrategyRunner as the fill handler.
    if (!gateway_.connect(*this)) {
        return false;
    }

    // Subscribe to all events on all symbols.
    const auto feed_status = feed_.subscribe(*this, "", market::SubscriptionMask::All);
    if (feed_status != market::FeedStatus::Ok) {
        gateway_.disconnect();
        return false;
    }

    running_ = true;
    strategy_.on_start(*this);

    // Start the feed (for SimGateway + MockFeed: synchronous dispatch).
    (void)feed_.start();
    return true;
}

void StrategyRunner::stop() noexcept {
    if (!running_) {
        return;
    }
    running_ = false;
    (void)feed_.stop();
    (void)feed_.unsubscribe(*this, "");
    strategy_.on_stop();
    gateway_.disconnect();
}

auto StrategyRunner::is_running() const noexcept -> bool {
    return running_;
}

// ---------------------------------------------------------------------------
// IOrderRouter::submit
// ---------------------------------------------------------------------------
auto StrategyRunner::submit(execution::OrderRequest request) noexcept -> OrderResult {
    // Assign client_order_id — strategy must not set this.
    request.client_order_id = next_order_id_++;

    // Get per-symbol portfolio view for risk check.
    const auto pv = portfolio_.snapshot_for(request.symbol);

    // Pre-trade risk gate.
    const auto verdict = risk_.validate(request, pv);
    if (!verdict.approved) {
        ++orders_rejected_by_risk_;
        return OrderResult{
            .submitted = false,
            .risk_verdict = verdict,
            .gateway_ack = execution::OrderAck{.client_order_id = request.client_order_id,
                                               .status = execution::GatewayStatus::Rejected}};
    }

    // Record the request so on_fill() can look up the symbol.
    pending_orders_.emplace(request.client_order_id, request);
    ++orders_submitted_;

    // Submit to gateway.
    const auto ack = gateway_.submit_order(request);
    // If the gateway itself rejected (e.g. not connected), remove from pending.
    if (!ack.accepted()) {
        pending_orders_.erase(request.client_order_id);
        --orders_submitted_;
    }

    return OrderResult{.submitted = true, .risk_verdict = verdict, .gateway_ack = ack};
}

auto StrategyRunner::cancel(const execution::CancelRequest& request) noexcept
    -> execution::OrderAck {
    return gateway_.cancel_order(request);
}

auto StrategyRunner::modify(const execution::ModifyRequest& request) noexcept
    -> execution::OrderAck {
    return gateway_.modify_order(request);
}

auto StrategyRunner::portfolio_view(const market::SymbolArray& symbol) const noexcept
    -> risk::PortfolioView {
    return portfolio_.snapshot_for(symbol);
}

auto StrategyRunner::is_halted() const noexcept -> bool {
    return risk_.is_halted();
}

// ---------------------------------------------------------------------------
// EventHandlerBase — market data callbacks
// ---------------------------------------------------------------------------
void StrategyRunner::on_event(const market::MarketEvent& event) noexcept {
    // Mark-to-market the portfolio on Quote and Tick events.
    if (const auto* q = std::get_if<market::Quote>(&event)) {
        const auto mid = (q->bid_price + q->ask_price) / 2;
        portfolio_.mark_to_market(q->symbol, mid);
    } else if (const auto* t = std::get_if<market::Tick>(&event)) {
        portfolio_.mark_to_market(t->symbol, t->price);
    }

    // Forward to strategy.
    strategy_.on_market_event(event);
}

void StrategyRunner::on_connected() noexcept {}
void StrategyRunner::on_disconnected() noexcept {}
void StrategyRunner::on_error(std::string_view /*reason*/) noexcept {}

// ---------------------------------------------------------------------------
// IFillHandler — fill callbacks (called by SimGateway synchronously)
// ---------------------------------------------------------------------------
void StrategyRunner::on_fill(const core::ExecutionReport& report) noexcept {
    // Look up the symbol from the original OrderRequest.
    const auto it = pending_orders_.find(report.order_id);
    if (it != pending_orders_.end()) {
        portfolio_.apply_fill_with_symbol(report, it->second.symbol);

        // Clean up terminal states.
        if (report.status == core::OrderStatus::Filled ||
            report.status == core::OrderStatus::Cancelled ||
            report.status == core::OrderStatus::Rejected) {
            pending_orders_.erase(it);
        }
    }

    // Forward to strategy.
    strategy_.on_fill(report);
}

void StrategyRunner::on_gateway_connected() noexcept {}
void StrategyRunner::on_gateway_disconnected(std::string_view /*r*/) noexcept {}

}  // namespace quantengine::strategy
