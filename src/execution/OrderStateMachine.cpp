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

// src/execution/OrderStateMachine.cpp
//
// M11 OrderStateMachine implementation.

#include "quantengine/execution/OrderStateMachine.hpp"

namespace quantengine::execution {

bool OrderStateMachine::is_valid_transition(InFlightState from, InFlightState to) noexcept {
    if (from == to) {
        return true;
    }

    switch (from) {
        case InFlightState::New:
            return to == InFlightState::PendingAck || to == InFlightState::Rejected;

        case InFlightState::PendingAck:
            return to == InFlightState::Resting || to == InFlightState::PartiallyFilled ||
                   to == InFlightState::Filled || to == InFlightState::Rejected ||
                   to == InFlightState::Cancelled;

        case InFlightState::Resting:
            return to == InFlightState::PartiallyFilled || to == InFlightState::Filled ||
                   to == InFlightState::PendingCancel || to == InFlightState::PendingModify ||
                   to == InFlightState::Cancelled;

        case InFlightState::PartiallyFilled:
            return to == InFlightState::PartiallyFilled || to == InFlightState::Filled ||
                   to == InFlightState::PendingCancel || to == InFlightState::PendingModify ||
                   to == InFlightState::Cancelled;

        case InFlightState::PendingCancel:
            return to == InFlightState::Cancelled || to == InFlightState::Filled ||
                   to == InFlightState::Resting || to == InFlightState::PartiallyFilled;

        case InFlightState::PendingModify:
            return to == InFlightState::Resting || to == InFlightState::PartiallyFilled ||
                   to == InFlightState::Filled || to == InFlightState::Cancelled;

        case InFlightState::Filled:
        case InFlightState::Cancelled:
        case InFlightState::Rejected:
            // Terminal states cannot transition
            return false;
    }

    return false;
}

bool OrderStateMachine::register_order(const OrderRequest& req,
                                       InFlightState initial_state) noexcept {
    const std::lock_guard<std::mutex> lock(mutex_);

    if (orders_.contains(req.client_order_id)) {
        return false;  // Duplicate client order id
    }

    TrackedOrder tracked{};
    tracked.client_order_id = req.client_order_id;
    tracked.request = req;
    tracked.state = initial_state;
    tracked.remaining_quantity = req.quantity;
    tracked.filled_quantity = 0;

    orders_.emplace(req.client_order_id, std::move(tracked));
    return true;
}

bool OrderStateMachine::associate_venue_id(core::OrderId client_order_id,
                                           std::string_view venue_id) noexcept {
    const std::lock_guard<std::mutex> lock(mutex_);

    auto it = orders_.find(client_order_id);
    if (it == orders_.end()) {
        return false;
    }

    it->second.venue_order_id = venue_id;
    venue_id_map_[std::string(venue_id)] = client_order_id;
    return true;
}

bool OrderStateMachine::transition(core::OrderId client_order_id,
                                   InFlightState new_state) noexcept {
    const std::lock_guard<std::mutex> lock(mutex_);

    auto it = orders_.find(client_order_id);
    if (it == orders_.end()) {
        return false;
    }

    if (!is_valid_transition(it->second.state, new_state)) {
        return false;
    }

    it->second.state = new_state;
    return true;
}

bool OrderStateMachine::apply_fill(core::OrderId client_order_id,
                                   core::Quantity fill_qty) noexcept {
    const std::lock_guard<std::mutex> lock(mutex_);

    auto it = orders_.find(client_order_id);
    if (it == orders_.end()) {
        return false;
    }

    auto& ord = it->second;
    if (fill_qty > ord.remaining_quantity) {
        fill_qty = ord.remaining_quantity;
    }

    ord.filled_quantity += fill_qty;
    ord.remaining_quantity -= fill_qty;

    if (ord.remaining_quantity == 0) {
        ord.state = InFlightState::Filled;
    } else {
        ord.state = InFlightState::PartiallyFilled;
    }

    return true;
}

std::optional<TrackedOrder> OrderStateMachine::get_order(
    core::OrderId client_order_id) const noexcept {
    const std::lock_guard<std::mutex> lock(mutex_);
    auto it = orders_.find(client_order_id);
    if (it != orders_.end()) {
        return it->second;
    }
    return std::nullopt;
}

std::optional<core::OrderId> OrderStateMachine::find_by_venue_id(
    std::string_view venue_id) const noexcept {
    const std::lock_guard<std::mutex> lock(mutex_);
    auto it = venue_id_map_.find(std::string(venue_id));
    if (it != venue_id_map_.end()) {
        return it->second;
    }
    return std::nullopt;
}

std::size_t OrderStateMachine::active_count() const noexcept {
    const std::lock_guard<std::mutex> lock(mutex_);
    std::size_t count = 0;
    for (const auto& [id, ord] : orders_) {
        if (!is_terminal(ord.state)) {
            ++count;
        }
    }
    return count;
}

std::size_t OrderStateMachine::pending_ack_count() const noexcept {
    const std::lock_guard<std::mutex> lock(mutex_);
    std::size_t count = 0;
    for (const auto& [id, ord] : orders_) {
        if (ord.state == InFlightState::PendingAck) {
            ++count;
        }
    }
    return count;
}

std::size_t OrderStateMachine::resting_count() const noexcept {
    const std::lock_guard<std::mutex> lock(mutex_);
    std::size_t count = 0;
    for (const auto& [id, ord] : orders_) {
        if (ord.state == InFlightState::Resting || ord.state == InFlightState::PartiallyFilled) {
            ++count;
        }
    }
    return count;
}

std::size_t OrderStateMachine::purge_terminal_orders() noexcept {
    const std::lock_guard<std::mutex> lock(mutex_);
    std::size_t purged = 0;

    for (auto it = orders_.begin(); it != orders_.end();) {
        if (is_terminal(it->second.state)) {
            if (!it->second.venue_order_id.empty()) {
                venue_id_map_.erase(it->second.venue_order_id);
            }
            it = orders_.erase(it);
            ++purged;
        } else {
            ++it;
        }
    }

    return purged;
}

void OrderStateMachine::reset() noexcept {
    const std::lock_guard<std::mutex> lock(mutex_);
    orders_.clear();
    venue_id_map_.clear();
}

}  // namespace quantengine::execution
