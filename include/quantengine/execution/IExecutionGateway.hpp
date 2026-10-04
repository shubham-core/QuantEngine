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

// quantengine/execution/IExecutionGateway.hpp
//
// Abstract interface for order routing and execution.
//
// DESIGN RATIONALE:
//   The gateway sits between the strategy+risk layer and the actual execution
//   venue (SimGateway -> OptimizedMatchingEngine, or AlpacaGateway -> broker).
//   It must present an identical interface to both, so the strategy is
//   completely unaware of whether it is paper trading or live.
//
//   Two patterns for fill notification were considered:
//
//   A) Synchronous return: submit_order() returns ExecutionReport directly.
//      Works for SimGateway (synchronous engine). Fails for live brokers
//      (fill arrives asynchronously on a different network event).
//
//   B) Callback (IFillHandler): submit_order() returns an acknowledgement
//      (order accepted / rejected), then calls handler.on_fill() later.
//      Works for both sim and live. Strategy must be stateful.
//
//   DECISION: Design B. submit_order() returns an OrderAck (synchronous
//   confirmation that the gateway accepted the order into its internal queue).
//   The actual fill — partial or complete — arrives via IFillHandler::on_fill()
//   from the gateway's event loop. For SimGateway, on_fill() is called
//   synchronously within submit_order() before it returns; the interface
//   contract is identical.
//
// THREAD SAFETY:
//   - submit_order(), cancel_order(), modify_order(): MAY be called from the
//     strategy thread. Implementations must be thread-safe for these methods.
//   - IFillHandler callbacks: called from the gateway's internal thread.
//     The handler MUST be re-entrant and MUST NOT block.
//   - connect() / disconnect(): setup/teardown only, called from the main
//     thread. NOT safe to call concurrently with submit_order().
//
// LIFETIME:
//   The IFillHandler reference passed to connect() must remain valid until
//   disconnect() is called. No reference counting — caller owns lifetime.
//
// IN-FLIGHT ORDER STATE MACHINE (P2 work — documented here as a contract):
//   New -> PendingAck -> [Resting | Filled | Rejected]
//          Resting   -> PendingCancel -> Cancelled
//          Resting   -> PartiallyFilled -> [Resting | Filled]
//   An order is "in-flight" between New and PendingAck.
//   The gateway must assign a client_order_id before submit to enable
//   idempotent resubmission after reconnect (prevents double-fill).

#include <cstdint>
#include <string_view>

#include "quantengine/core/events.hpp"
#include "quantengine/core/types.hpp"
#include "quantengine/market/MarketEvent.hpp"

namespace quantengine::execution {

// ---------------------------------------------------------------------------
// OrderRequest: the data contract for submitting a new order.
// 72 bytes — stack-allocated, zero heap.
//
// client_order_id: assigned by the caller (strategy or risk manager) BEFORE
//   submission. Must be unique and deterministic (e.g. hash of strategy_id +
//   instrument + session_seq). Used for idempotent resubmission after
//   reconnect. The gateway maps client_order_id -> venue_order_id internally.
//
// time_in_force: only Day and IOC are required for paper trading.
//   GTC requires persistent state across sessions (P2 scope).
// ---------------------------------------------------------------------------
enum class TimeInForce : std::uint8_t {
    Day = 0,  // cancel at end of session
    Ioc = 1,  // immediate-or-cancel: fill what you can, cancel remainder
    Gtc = 2   // good-till-cancel (P2: requires persistence across restart)
};

[[nodiscard]] constexpr auto to_string(TimeInForce tif) noexcept -> std::string_view {
    switch (tif) {
        case TimeInForce::Day:
            return "DAY";
        case TimeInForce::Ioc:
            return "IOC";
        case TimeInForce::Gtc:
            return "GTC";
    }
    return "UNKNOWN";
}

struct OrderRequest {
    core::OrderId client_order_id{0};  // assigned by caller; unique per session
    core::Side side{core::Side::Buy};
    core::OrderType type{core::OrderType::Limit};
    TimeInForce time_in_force{TimeInForce::Day};
    std::uint8_t pad[4]{};
    market::SymbolArray symbol{};  // 16 bytes
    core::PriceTicks price{0};     // 0 = market order (only if type = Market)
    core::Quantity quantity{0};

    [[nodiscard]] constexpr auto operator==(const OrderRequest&) const noexcept -> bool = default;
};
static_assert(sizeof(OrderRequest) == 48, "OrderRequest layout changed — update docs");
static_assert(alignof(OrderRequest) == 8);

// ---------------------------------------------------------------------------
// OrderAck: synchronous response to submit_order().
// Indicates whether the gateway accepted the order into its internal queue.
// This is NOT a fill confirmation — fills arrive via IFillHandler::on_fill().
//
// GatewayStatus::Accepted means the order entered the gateway's state machine.
// GatewayStatus::Rejected means the order was rejected before leaving the
//   process (risk check failed, invalid parameters, not connected, etc.).
// ---------------------------------------------------------------------------
enum class GatewayStatus : std::uint8_t {
    Accepted = 0,  // order entered gateway state machine
    Rejected,      // pre-submission rejection (see reject_reason)
    NotConnected,  // gateway is not connected to the venue
    RateLimited,   // order rate limit exceeded
    InternalError  // unexpected internal failure
};

[[nodiscard]] constexpr auto to_string(GatewayStatus s) noexcept -> std::string_view {
    switch (s) {
        case GatewayStatus::Accepted:
            return "ACCEPTED";
        case GatewayStatus::Rejected:
            return "REJECTED";
        case GatewayStatus::NotConnected:
            return "NOT_CONNECTED";
        case GatewayStatus::RateLimited:
            return "RATE_LIMITED";
        case GatewayStatus::InternalError:
            return "INTERNAL_ERROR";
    }
    return "UNKNOWN";
}

struct OrderAck {
    core::OrderId client_order_id{0};
    GatewayStatus status{GatewayStatus::Rejected};
    core::RejectReason reject_reason{core::RejectReason::None};
    std::uint8_t pad[6]{};

    [[nodiscard]] constexpr auto accepted() const noexcept -> bool {
        return status == GatewayStatus::Accepted;
    }
    [[nodiscard]] constexpr auto operator==(const OrderAck&) const noexcept -> bool = default;
};
static_assert(sizeof(OrderAck) == 16, "OrderAck layout changed — update docs");

// ---------------------------------------------------------------------------
// CancelRequest / ModifyRequest: thin wrappers for cancel and modify.
// ---------------------------------------------------------------------------
struct CancelRequest {
    core::OrderId client_order_id{0};  // must match a previously submitted order
    market::SymbolArray symbol{};      // required by some venues for routing

    [[nodiscard]] constexpr auto operator==(const CancelRequest&) const noexcept -> bool = default;
};
static_assert(sizeof(CancelRequest) == 24);

struct ModifyRequest {
    core::OrderId client_order_id{0};
    core::PriceTicks new_price{0};
    core::Quantity new_quantity{0};
    market::SymbolArray symbol{};

    [[nodiscard]] constexpr auto operator==(const ModifyRequest&) const noexcept -> bool = default;
};
static_assert(sizeof(ModifyRequest) == 40);

// ---------------------------------------------------------------------------
// IFillHandler: callback interface for asynchronous fill notifications.
//
// Implement this on your Portfolio / Strategy and pass it to connect().
// All methods MUST be noexcept — a throw terminates the gateway thread.
// ---------------------------------------------------------------------------
class IFillHandler {
public:
    IFillHandler() = default;
    virtual ~IFillHandler() = default;

    IFillHandler(const IFillHandler&) = delete;
    auto operator=(const IFillHandler&) -> IFillHandler& = delete;
    IFillHandler(IFillHandler&&) = default;
    auto operator=(IFillHandler&&) -> IFillHandler& = default;

    // Called for every ExecutionReport (resting, partially filled, filled,
    // cancelled, rejected). client_order_id maps the report back to the
    // strategy's original OrderRequest.
    virtual void on_fill(const core::ExecutionReport& report) noexcept = 0;

    // Called when the venue connection is established.
    virtual void on_gateway_connected() noexcept = 0;

    // Called on disconnect. reason is "GRACEFUL" or an error description.
    // The gateway will attempt reconnect internally if configured to do so.
    virtual void on_gateway_disconnected(std::string_view reason) noexcept = 0;
};

// ---------------------------------------------------------------------------
// IExecutionGateway: abstract execution interface.
//
// Implementations:
//   SimGateway       — routes to OptimizedMatchingEngine synchronously
//   AlpacaGateway    — routes to Alpaca REST + WebSocket (P2 scope)
//
// CONTRACT:
//   connect(handler) before submit_order().
//   disconnect() before destroying the gateway.
//   client_order_id must be unique per session; the gateway does NOT
//   deduplicate — that is the caller's responsibility (see IRiskManager).
// ---------------------------------------------------------------------------
class IExecutionGateway {
public:
    IExecutionGateway() = default;
    virtual ~IExecutionGateway() = default;

    IExecutionGateway(const IExecutionGateway&) = delete;
    auto operator=(const IExecutionGateway&) -> IExecutionGateway& = delete;
    IExecutionGateway(IExecutionGateway&&) = default;
    auto operator=(IExecutionGateway&&) -> IExecutionGateway& = default;

    // Establish connection to the venue and register the fill handler.
    // Returns false if already connected or connection fails.
    [[nodiscard]] virtual auto connect(IFillHandler& handler) noexcept -> bool = 0;

    // Disconnect gracefully. Blocks until in-flight orders are either filled
    // or cancelled (implementation-defined timeout applies for live gateways).
    virtual void disconnect() noexcept = 0;

    [[nodiscard]] virtual auto is_connected() const noexcept -> bool = 0;

    // Submit a new order. Returns an OrderAck synchronously.
    // Fills (partial or complete) arrive via IFillHandler::on_fill().
    // For SimGateway: on_fill() is called synchronously before this returns.
    // For live gateways: on_fill() arrives on the network event thread.
    [[nodiscard]] virtual auto submit_order(const OrderRequest& request) noexcept -> OrderAck = 0;

    // Cancel a resting order. Returns an OrderAck indicating whether the
    // cancel was queued. The actual cancellation confirmation arrives via
    // IFillHandler::on_fill() with status = Cancelled.
    [[nodiscard]] virtual auto cancel_order(const CancelRequest& request) noexcept -> OrderAck = 0;

    // Modify a resting order. Semantics: cancel-then-reinsert internally.
    // Returns Rejected with ModifyCrossesSpread if the new price crosses the
    // opposing best (mirrors engine-level protection from Milestone 1).
    [[nodiscard]] virtual auto modify_order(const ModifyRequest& request) noexcept -> OrderAck = 0;

    // Human-readable gateway identifier for logging.
    [[nodiscard]] virtual auto gateway_name() const noexcept -> std::string_view = 0;
};

}  // namespace quantengine::execution
