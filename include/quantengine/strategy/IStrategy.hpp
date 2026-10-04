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

// quantengine/strategy/IStrategy.hpp
//
// M8: Abstract strategy interface and IOrderRouter.
//
// DESIGN:
//   IOrderRouter is the minimal API a strategy uses to interact with the
//   market. It is implemented by StrategyRunner, which sits between the
//   strategy and the risk/gateway layer. The strategy never touches the risk
//   manager or gateway directly.
//
//   IStrategy receives market events and fills, and uses IOrderRouter to
//   submit, cancel, or modify orders.
//
//   StrategyRunner wires everything together:
//     Feed → EventHandlerBase::on_event() → IStrategy::on_market_event()
//     IStrategy → IOrderRouter::submit() → RiskManager → Gateway
//     Gateway  → IFillHandler::on_fill() → Portfolio + IStrategy::on_fill()
//
// THREAD SAFETY:
//   All IStrategy methods are called from a single thread (the strategy thread
//   or the feed's dispatch thread). Implementations do NOT need to be
//   internally thread-safe unless they share state with other components.
//
//   IOrderRouter methods may be called from any thread that owns the strategy.
//   StrategyRunner's implementation is NOT thread-safe — strategies must not
//   call IOrderRouter from a background thread.

#include <cstdint>
#include <string_view>

#include "quantengine/core/events.hpp"
#include "quantengine/execution/IExecutionGateway.hpp"
#include "quantengine/market/MarketEvent.hpp"
#include "quantengine/risk/IRiskManager.hpp"

namespace quantengine::strategy {

// ---------------------------------------------------------------------------
// OrderResult: returned by IOrderRouter::submit() / cancel() / modify().
// Bundles the risk verdict and gateway acknowledgement in one value.
// ---------------------------------------------------------------------------
struct OrderResult {
    bool submitted{false};  // true iff order reached gateway
    risk::RiskVerdict risk_verdict{};
    execution::OrderAck gateway_ack{};

    [[nodiscard]] auto accepted() const noexcept -> bool {
        return submitted && gateway_ack.accepted();
    }
};

// ---------------------------------------------------------------------------
// IOrderRouter: the strategy's handle to submit/cancel/modify orders.
// Implemented by StrategyRunner. Passed to IStrategy::on_start().
//
// ORDER ID ASSIGNMENT:
//   The router assigns monotonically increasing client_order_ids internally.
//   The strategy does NOT set client_order_id — it receives the assigned id
//   in the OrderAck and subsequent fill reports.
// ---------------------------------------------------------------------------
class IOrderRouter {
public:
    IOrderRouter() = default;
    virtual ~IOrderRouter() = default;

    IOrderRouter(const IOrderRouter&) = delete;
    auto operator=(const IOrderRouter&) -> IOrderRouter& = delete;
    IOrderRouter(IOrderRouter&&) = default;
    auto operator=(IOrderRouter&&) -> IOrderRouter& = default;

    // Submit a new order. client_order_id is IGNORED in the request — the
    // router assigns one and overwrites the field before calling risk + gateway.
    // The assigned id is returned in OrderResult::gateway_ack.client_order_id.
    [[nodiscard]] virtual auto submit(execution::OrderRequest request) noexcept -> OrderResult = 0;

    // Cancel a resting order. client_order_id must match a previously
    // submitted order's assigned id (from OrderAck).
    [[nodiscard]] virtual auto cancel(const execution::CancelRequest& request) noexcept
        -> execution::OrderAck = 0;

    // Modify a resting order.
    [[nodiscard]] virtual auto modify(const execution::ModifyRequest& request) noexcept
        -> execution::OrderAck = 0;

    // Read-only snapshot of the portfolio for the specified symbol.
    [[nodiscard]] virtual auto portfolio_view(const market::SymbolArray& symbol) const noexcept
        -> risk::PortfolioView = 0;

    // Whether the risk manager is currently halted (drawdown etc.).
    [[nodiscard]] virtual auto is_halted() const noexcept -> bool = 0;
};

// ---------------------------------------------------------------------------
// IStrategy: abstract strategy interface.
//
// LIFECYCLE:
//   on_start(router): called once before the first market event.
//     Strategy should initialise state and store the router reference.
//   on_market_event(ev): called for every market event from the feed.
//     Hot path — must be fast, no blocking, no dynamic allocation on critical path.
//   on_fill(report): called for every ExecutionReport from the gateway.
//   on_stop(): called when the feed/session is stopping. Clean up open orders.
// ---------------------------------------------------------------------------
class IStrategy {
public:
    IStrategy() = default;
    virtual ~IStrategy() = default;

    IStrategy(const IStrategy&) = delete;
    auto operator=(const IStrategy&) -> IStrategy& = delete;
    IStrategy(IStrategy&&) = default;
    auto operator=(IStrategy&&) -> IStrategy& = default;

    // Called once before events start. router is valid until on_stop() returns.
    virtual void on_start(IOrderRouter& router) noexcept = 0;

    // Hot path: called for every market event delivered by the feed.
    virtual void on_market_event(const market::MarketEvent& event) noexcept = 0;

    // Called for every ExecutionReport from the gateway (filled, cancelled, etc.).
    virtual void on_fill(const core::ExecutionReport& report) noexcept = 0;

    // Called when the session stops. Strategy should clean up state.
    virtual void on_stop() noexcept = 0;

    [[nodiscard]] virtual auto name() const noexcept -> std::string_view = 0;
};

}  // namespace quantengine::strategy
