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

#include <iosfwd>

#include "quantengine/core/types.hpp"

namespace quantengine::core {

class Order {
public:
    constexpr Order() noexcept = default;

    constexpr Order(OrderId id, Side side, PriceTicks price, Quantity quantity,
                    SeqNum seq = 0) noexcept
        : order_id_(id),
          side_(side),
          price_(price),
          initial_quantity_(quantity),
          remaining_quantity_(quantity),
          sequence_number_(seq),
          status_(OrderStatus::New) {}

    [[nodiscard]] constexpr OrderId order_id() const noexcept { return order_id_; }
    [[nodiscard]] constexpr Side side() const noexcept { return side_; }
    [[nodiscard]] constexpr PriceTicks price() const noexcept { return price_; }
    [[nodiscard]] constexpr Quantity initial_quantity() const noexcept { return initial_quantity_; }
    [[nodiscard]] constexpr Quantity remaining_quantity() const noexcept {
        return remaining_quantity_;
    }
    [[nodiscard]] constexpr Quantity filled_quantity() const noexcept {
        return initial_quantity_ - remaining_quantity_;
    }
    [[nodiscard]] constexpr SeqNum sequence_number() const noexcept { return sequence_number_; }
    [[nodiscard]] constexpr OrderStatus status() const noexcept { return status_; }

    [[nodiscard]] constexpr bool is_active() const noexcept {
        return status_ == OrderStatus::Resting || status_ == OrderStatus::PartiallyFilled;
    }

    [[nodiscard]] constexpr bool is_terminal() const noexcept {
        return status_ == OrderStatus::Filled || status_ == OrderStatus::Cancelled ||
               status_ == OrderStatus::Rejected;
    }

    // State machine transitions
    [[nodiscard]] bool mark_resting() noexcept;
    [[nodiscard]] bool apply_fill(Quantity fill_qty) noexcept;
    [[nodiscard]] bool mark_cancelled() noexcept;
    [[nodiscard]] bool mark_rejected() noexcept;

    // Invariants
    [[nodiscard]] bool check_invariants() const noexcept;

    // Modification helpers
    void update_priority_sequence(SeqNum new_seq) noexcept { sequence_number_ = new_seq; }
    void update_price(PriceTicks new_price) noexcept { price_ = new_price; }
    void update_remaining_quantity(Quantity new_qty) noexcept {
        remaining_quantity_ = new_qty;
        if (new_qty > initial_quantity_) {
            initial_quantity_ = new_qty;
        }
    }

    [[nodiscard]] constexpr bool operator==(const Order& other) const noexcept = default;

private:
    OrderId order_id_{0};
    Side side_{Side::Buy};
    PriceTicks price_{0};
    Quantity initial_quantity_{0};
    Quantity remaining_quantity_{0};
    SeqNum sequence_number_{0};
    OrderStatus status_{OrderStatus::New};
};

std::ostream& operator<<(std::ostream& os, const Order& order);

// Layout guard: if Quantity or any field type changes, update this assert and document the
// new cache-line density. Current layout on x86-64 (GCC 13, Quantity=uint64_t):
//   order_id(8) | side(1)+7pad | price(8) | init_qty(8) | rem_qty(8) | seq(8) | status(1)+7pad
//   = 56 bytes  => 1.14 OrderNode per 64-byte cache line (see optimized_order_book.hpp)
static_assert(sizeof(Order) == 56,
              "core::Order layout changed — update OrderNode static_assert and cache-line docs");

}  // namespace quantengine::core
