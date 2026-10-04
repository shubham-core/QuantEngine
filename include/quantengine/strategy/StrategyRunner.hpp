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

// quantengine/strategy/StrategyRunner.hpp
//
// M8: Wires IMarketDataFeed → IStrategy → IRiskManager → IExecutionGateway
// into a single event loop. Also fans fills to Portfolio and IStrategy.
//
// ARCHITECTURE:
//
//   StrategyRunner owns:
//     IMarketDataFeed&    feed_     — market data source
//     IRiskManager&       risk_     — pre-trade risk gate
//     IExecutionGateway&  gateway_  — order routing
//     Portfolio&          portfolio_ — P&L ledger
//     IStrategy&          strategy_ — the trading logic
//
//   StrategyRunner implements:
//     EventHandlerBase   → registered with feed_ on run()
//     IFillHandler       → registered with gateway_ on run()
//     IOrderRouter       → passed to strategy_.on_start()
//
//   Event flow:
//     1. feed_.start() triggers EventHandlerBase::on_event()
//        a. portfolio_.mark_to_market() for Quote events
//        b. strategy_.on_market_event()
//     2. strategy_ calls IOrderRouter::submit() (= StrategyRunner::submit())
//        a. StrategyRunner assigns next client_order_id
//        b. risk_.validate(request, portfolio_.snapshot_for(symbol))
//        c. If approved: gateway_.submit_order(request) → triggers on_fill()
//        d. Returns OrderResult to strategy
//     3. IFillHandler::on_fill() (called synchronously by SimGateway)
//        a. portfolio_.apply_fill_with_symbol()
//        b. strategy_.on_fill()
//
// THREAD SAFETY:
//   StrategyRunner is designed for a single-threaded event loop (SimGateway).
//   For live gateways, the gateway's network thread calls on_fill() — in that
//   case a command queue between on_fill() and the strategy thread is needed
//   (P2 scope). The Portfolio uses a mutex internally for safety regardless.
//
// RUN SEMANTICS:
//   run() is synchronous for the simulation case: it starts the feed, delivers
//   all events already in the feed's queue (for CsvReplayFeed), then stops.
//   For streaming feeds it runs until stop() is called from another thread.
//   step() delivers exactly one event — used in tests for fine-grained control.

#include <atomic>
#include <cstdint>
#include <unordered_map>

#include "quantengine/execution/IExecutionGateway.hpp"
#include "quantengine/market/IMarketDataFeed.hpp"
#include "quantengine/portfolio/Portfolio.hpp"
#include "quantengine/risk/IRiskManager.hpp"
#include "quantengine/strategy/IStrategy.hpp"

namespace quantengine::strategy {

class StrategyRunner final : public market::EventHandlerBase,  // market data callbacks
                             public execution::IFillHandler,   // fill callbacks
                             public IOrderRouter {             // order submission for strategy

public:
    StrategyRunner(market::IMarketDataFeed& feed, risk::IRiskManager& risk,
                   execution::IExecutionGateway& gateway, portfolio::Portfolio& portfolio,
                   IStrategy& strategy);

    ~StrategyRunner() override;

    StrategyRunner(const StrategyRunner&) = delete;
    auto operator=(const StrategyRunner&) -> StrategyRunner& = delete;
    StrategyRunner(StrategyRunner&&) = delete;
    auto operator=(StrategyRunner&&) -> StrategyRunner& = delete;

    // ---- Lifecycle ----------------------------------------------------------

    // Connect gateway, subscribe feed, call strategy.on_start(), start feed.
    // For SimGateway + MockFeed: returns after all events are dispatched.
    // Returns true if startup succeeded (gateway connected, feed started).
    [[nodiscard]] auto run() noexcept -> bool;

    // Stop the feed and call strategy.on_stop(). Idempotent.
    void stop() noexcept;

    [[nodiscard]] auto is_running() const noexcept -> bool;

    // ---- IOrderRouter -------------------------------------------------------

    [[nodiscard]] auto submit(execution::OrderRequest request) noexcept -> OrderResult override;

    [[nodiscard]] auto cancel(const execution::CancelRequest& request) noexcept
        -> execution::OrderAck override;

    [[nodiscard]] auto modify(const execution::ModifyRequest& request) noexcept
        -> execution::OrderAck override;

    [[nodiscard]] auto portfolio_view(const market::SymbolArray& symbol) const noexcept
        -> risk::PortfolioView override;

    [[nodiscard]] auto is_halted() const noexcept -> bool override;

    // ---- EventHandlerBase (IEventHandler concept) ---------------------------

    void on_event(const market::MarketEvent& event) noexcept override;
    void on_connected() noexcept override;
    void on_disconnected() noexcept override;
    void on_error(std::string_view reason) noexcept override;

    // ---- IFillHandler -------------------------------------------------------

    void on_fill(const core::ExecutionReport& report) noexcept override;
    void on_gateway_connected() noexcept override;
    void on_gateway_disconnected(std::string_view reason) noexcept override;

    // ---- Diagnostics --------------------------------------------------------

    [[nodiscard]] auto orders_submitted() const noexcept -> std::uint64_t {
        return orders_submitted_;
    }
    [[nodiscard]] auto orders_rejected_by_risk() const noexcept -> std::uint64_t {
        return orders_rejected_by_risk_;
    }

private:
    market::IMarketDataFeed& feed_;
    risk::IRiskManager& risk_;
    execution::IExecutionGateway& gateway_;
    portfolio::Portfolio& portfolio_;
    IStrategy& strategy_;

    // Monotonically increasing client_order_id assigned by the runner.
    std::uint64_t next_order_id_{1};

    // Map from assigned client_order_id → original OrderRequest (for symbol lookup).
    // Allocated at order time (not on the event-dispatch hot path).
    std::unordered_map<core::OrderId, execution::OrderRequest> pending_orders_;

    // Metrics
    std::uint64_t orders_submitted_{0};
    std::uint64_t orders_rejected_by_risk_{0};

    bool running_{false};
};

}  // namespace quantengine::strategy
