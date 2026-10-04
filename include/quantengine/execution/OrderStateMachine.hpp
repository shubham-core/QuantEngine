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

// quantengine/execution/OrderStateMachine.hpp
//
// M11: In-flight order state machine and idempotency key manager.
// Ensures strict valid state transitions for broker gateways, detects
// timeouts, and manages client_order_id <-> venue_order_id mappings.

#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

#include "quantengine/core/types.hpp"
#include "quantengine/execution/IExecutionGateway.hpp"

namespace quantengine::execution {

// ---------------------------------------------------------------------------
// InFlightState: lifecycle states of an order in flight to a broker/venue.
// ---------------------------------------------------------------------------
enum class InFlightState : std::uint8_t {
    New = 0,          // Created locally, not yet sent
    PendingAck,       // Sent to gateway/venue, awaiting acknowledgement
    Resting,          // Confirmed on venue order book
    PartiallyFilled,  // Partially matched, remainder still resting
    PendingCancel,    // Cancel request dispatched, awaiting confirmation
    PendingModify,    // Modify request dispatched, awaiting confirmation
    Filled,           // Completely filled (terminal)
    Cancelled,        // Cancel confirmed (terminal)
    Rejected          // Venue rejected order (terminal)
};

[[nodiscard]] constexpr auto to_string(InFlightState s) noexcept -> std::string_view {
    switch (s) {
        case InFlightState::New:
            return "NEW";
        case InFlightState::PendingAck:
            return "PENDING_ACK";
        case InFlightState::Resting:
            return "RESTING";
        case InFlightState::PartiallyFilled:
            return "PARTIALLY_FILLED";
        case InFlightState::PendingCancel:
            return "PENDING_CANCEL";
        case InFlightState::PendingModify:
            return "PENDING_MODIFY";
        case InFlightState::Filled:
            return "FILLED";
        case InFlightState::Cancelled:
            return "CANCELLED";
        case InFlightState::Rejected:
            return "REJECTED";
    }
    return "UNKNOWN";
}

[[nodiscard]] constexpr bool is_terminal(InFlightState s) noexcept {
    return s == InFlightState::Filled || s == InFlightState::Cancelled ||
           s == InFlightState::Rejected;
}

// ---------------------------------------------------------------------------
// TrackedOrder: metadata for an active or historical order in flight
// ---------------------------------------------------------------------------
struct TrackedOrder {
    core::OrderId client_order_id{0};
    std::string venue_order_id{};
    OrderRequest request{};
    InFlightState state{InFlightState::New};
    core::Quantity filled_quantity{0};
    core::Quantity remaining_quantity{0};
    std::int64_t last_update_ts{0};
    std::uint32_t retry_count{0};
};

// ---------------------------------------------------------------------------
// OrderStateMachine: thread-safe manager for tracking orders in flight
// ---------------------------------------------------------------------------
class OrderStateMachine {
public:
    OrderStateMachine() = default;
    ~OrderStateMachine() = default;

    OrderStateMachine(const OrderStateMachine&) = delete;
    auto operator=(const OrderStateMachine&) -> OrderStateMachine& = delete;
    OrderStateMachine(OrderStateMachine&&) = default;
    auto operator=(OrderStateMachine&&) -> OrderStateMachine& = default;

    // Validate if a transition from `from` to `to` is legally allowed
    [[nodiscard]] static bool is_valid_transition(InFlightState from, InFlightState to) noexcept;

    // Register a new outgoing order (starts in New or PendingAck)
    bool register_order(const OrderRequest& req,
                        InFlightState initial_state = InFlightState::PendingAck) noexcept;

    // Link a venue_order_id (e.g. from broker ACK) to an existing client_order_id
    bool associate_venue_id(core::OrderId client_order_id, std::string_view venue_id) noexcept;

    // Transition an order to a new state
    bool transition(core::OrderId client_order_id, InFlightState new_state) noexcept;

    // Update fill progress
    bool apply_fill(core::OrderId client_order_id, core::Quantity fill_qty) noexcept;

    // Lookups
    [[nodiscard]] std::optional<TrackedOrder> get_order(
        core::OrderId client_order_id) const noexcept;
    [[nodiscard]] std::optional<core::OrderId> find_by_venue_id(
        std::string_view venue_id) const noexcept;

    // Active order queries
    [[nodiscard]] std::size_t active_count() const noexcept;
    [[nodiscard]] std::size_t pending_ack_count() const noexcept;
    [[nodiscard]] std::size_t resting_count() const noexcept;

    // Clear completed (terminal) orders
    std::size_t purge_terminal_orders() noexcept;

    void reset() noexcept;

private:
    mutable std::mutex mutex_;
    std::unordered_map<core::OrderId, TrackedOrder> orders_;
    std::unordered_map<std::string, core::OrderId> venue_id_map_;
};

}  // namespace quantengine::execution
