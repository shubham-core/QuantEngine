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

#include <cstdint>
#include <iosfwd>
#include <string_view>

namespace quantengine::core {

using PriceTicks = std::int64_t;
using Quantity = std::uint64_t;
using OrderId = std::uint64_t;
using SeqNum = std::uint64_t;
using TradeId = std::uint64_t;

enum class Side : std::uint8_t { Buy = 0, Sell = 1 };

[[nodiscard]] constexpr Side opposite_side(Side side) noexcept {
    return (side == Side::Buy) ? Side::Sell : Side::Buy;
}

[[nodiscard]] constexpr std::string_view to_string(Side side) noexcept {
    switch (side) {
        case Side::Buy:
            return "BUY";
        case Side::Sell:
            return "SELL";
    }
    return "UNKNOWN";
}

std::ostream& operator<<(std::ostream& os, Side side);

enum class OrderStatus : std::uint8_t {
    New = 0,
    Resting,
    PartiallyFilled,
    Filled,
    Cancelled,
    Rejected
};

[[nodiscard]] constexpr std::string_view to_string(OrderStatus status) noexcept {
    switch (status) {
        case OrderStatus::New:
            return "NEW";
        case OrderStatus::Resting:
            return "RESTING";
        case OrderStatus::PartiallyFilled:
            return "PARTIALLY_FILLED";
        case OrderStatus::Filled:
            return "FILLED";
        case OrderStatus::Cancelled:
            return "CANCELLED";
        case OrderStatus::Rejected:
            return "REJECTED";
    }
    return "UNKNOWN";
}

std::ostream& operator<<(std::ostream& os, OrderStatus status);

enum class OrderType : std::uint8_t { Limit = 0 };

[[nodiscard]] constexpr std::string_view to_string(OrderType type) noexcept {
    switch (type) {
        case OrderType::Limit:
            return "LIMIT";
    }
    return "UNKNOWN";
}

std::ostream& operator<<(std::ostream& os, OrderType type);

enum class RejectReason : std::uint8_t {
    None = 0,
    DuplicateOrderId,
    UnknownOrder,
    InvalidPrice,
    InvalidQuantity,
    OrderAlreadyFilled,
    OrderAlreadyCancelled,
    InvalidStateTransition,
    // Modify rejected because the new price would immediately cross the opposing best,
    // turning a modification into an implicit aggressive order. Callers must explicitly
    // cancel-then-place if crossing execution is the intent.
    ModifyCrossesSpread
};

[[nodiscard]] constexpr std::string_view to_string(RejectReason reason) noexcept {
    switch (reason) {
        case RejectReason::None:
            return "NONE";
        case RejectReason::DuplicateOrderId:
            return "DUPLICATE_ORDER_ID";
        case RejectReason::UnknownOrder:
            return "UNKNOWN_ORDER";
        case RejectReason::InvalidPrice:
            return "INVALID_PRICE";
        case RejectReason::InvalidQuantity:
            return "INVALID_QUANTITY";
        case RejectReason::OrderAlreadyFilled:
            return "ORDER_ALREADY_FILLED";
        case RejectReason::OrderAlreadyCancelled:
            return "ORDER_ALREADY_CANCELLED";
        case RejectReason::InvalidStateTransition:
            return "INVALID_STATE_TRANSITION";
        case RejectReason::ModifyCrossesSpread:
            return "MODIFY_CROSSES_SPREAD";
    }
    return "UNKNOWN";
}

std::ostream& operator<<(std::ostream& os, RejectReason reason);

}  // namespace quantengine::core
