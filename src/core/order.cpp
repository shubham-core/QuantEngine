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

#include "quantengine/core/order.hpp"

#include <ostream>

namespace quantengine::core {

bool Order::mark_resting() noexcept {
    if (status_ == OrderStatus::New || status_ == OrderStatus::PartiallyFilled ||
        status_ == OrderStatus::Resting) {
        status_ = OrderStatus::Resting;
        return true;
    }
    return false;
}

bool Order::apply_fill(Quantity fill_qty) noexcept {
    if (fill_qty == 0 || fill_qty > remaining_quantity_) {
        return false;
    }

    if (status_ != OrderStatus::New && status_ != OrderStatus::Resting &&
        status_ != OrderStatus::PartiallyFilled) {
        return false;
    }

    remaining_quantity_ -= fill_qty;
    if (remaining_quantity_ == 0) {
        status_ = OrderStatus::Filled;
    } else {
        status_ = OrderStatus::PartiallyFilled;
    }
    return true;
}

bool Order::mark_cancelled() noexcept {
    if (status_ == OrderStatus::New || status_ == OrderStatus::Resting ||
        status_ == OrderStatus::PartiallyFilled) {
        remaining_quantity_ = 0;
        status_ = OrderStatus::Cancelled;
        return true;
    }
    return false;
}

bool Order::mark_rejected() noexcept {
    if (status_ == OrderStatus::New) {
        status_ = OrderStatus::Rejected;
        return true;
    }
    return false;
}

bool Order::check_invariants() const noexcept {
    if (order_id_ == 0 || price_ <= 0 || initial_quantity_ == 0) {
        return false;
    }

    if (remaining_quantity_ > initial_quantity_) {
        return false;
    }

    switch (status_) {
        case OrderStatus::New:
            return remaining_quantity_ == initial_quantity_;
        case OrderStatus::Resting:
            return remaining_quantity_ > 0 && remaining_quantity_ <= initial_quantity_;
        case OrderStatus::PartiallyFilled:
            return remaining_quantity_ > 0 && remaining_quantity_ < initial_quantity_;
        case OrderStatus::Filled:
            return remaining_quantity_ == 0;
        case OrderStatus::Cancelled:
            return remaining_quantity_ == 0;
        case OrderStatus::Rejected:
            return true;
    }
    return false;
}

std::ostream& operator<<(std::ostream& os, const Order& order) {
    return os << "Order{id=" << order.order_id() << ", side=" << order.side()
              << ", px=" << order.price() << ", rem=" << order.remaining_quantity() << "/"
              << order.initial_quantity() << ", status=" << order.status()
              << ", seq=" << order.sequence_number() << "}";
}

}  // namespace quantengine::core
