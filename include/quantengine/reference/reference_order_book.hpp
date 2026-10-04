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

#include <cstddef>
#include <functional>
#include <list>
#include <map>
#include <optional>
#include <unordered_map>
#include <vector>

#include "quantengine/core/order.hpp"
#include "quantengine/core/types.hpp"

namespace quantengine::reference {

struct LevelInfo {
    core::PriceTicks price{0};
    core::Quantity total_quantity{0};
    std::size_t order_count{0};

    [[nodiscard]] constexpr bool operator==(const LevelInfo&) const noexcept = default;
};

class ReferenceOrderBook {
public:
    ReferenceOrderBook() = default;
    ~ReferenceOrderBook() = default;

    ReferenceOrderBook(const ReferenceOrderBook&) = delete;
    ReferenceOrderBook& operator=(const ReferenceOrderBook&) = delete;
    ReferenceOrderBook(ReferenceOrderBook&&) noexcept = default;
    ReferenceOrderBook& operator=(ReferenceOrderBook&&) noexcept = default;

    // Core Order Book Operations
    [[nodiscard]] bool add_order(core::Order order);
    [[nodiscard]] std::optional<core::Order> cancel_order(core::OrderId order_id);
    [[nodiscard]] bool modify_order(core::OrderId order_id, core::PriceTicks new_price,
                                    core::Quantity new_quantity, core::SeqNum new_seq);

    // Queries
    [[nodiscard]] bool has_order(core::OrderId order_id) const noexcept;
    [[nodiscard]] const core::Order* get_order(core::OrderId order_id) const noexcept;
    [[nodiscard]] core::Order* get_order_mut(core::OrderId order_id) noexcept;

    [[nodiscard]] std::optional<core::PriceTicks> best_bid_price() const noexcept;
    [[nodiscard]] std::optional<core::PriceTicks> best_ask_price() const noexcept;

    [[nodiscard]] std::optional<core::Quantity> best_bid_quantity() const noexcept;
    [[nodiscard]] std::optional<core::Quantity> best_ask_quantity() const noexcept;

    // Matching Engine Accessors & Mutators
    [[nodiscard]] core::Order* get_best_bid_order() noexcept;
    [[nodiscard]] core::Order* get_best_ask_order() noexcept;
    void fill_best_bid_order(core::Quantity fill_qty) noexcept;
    void fill_best_ask_order(core::Quantity fill_qty) noexcept;
    void pop_best_bid_order() noexcept;
    void pop_best_ask_order() noexcept;

    // Depth and Inspection
    [[nodiscard]] std::size_t bid_level_count() const noexcept;
    [[nodiscard]] std::size_t ask_level_count() const noexcept;
    [[nodiscard]] std::size_t total_orders() const noexcept;
    [[nodiscard]] core::Quantity total_bid_volume() const noexcept;
    [[nodiscard]] core::Quantity total_ask_volume() const noexcept;
    [[nodiscard]] bool is_empty() const noexcept;
    void clear() noexcept;

    [[nodiscard]] std::vector<LevelInfo> get_bids(std::size_t max_levels = 0) const;
    [[nodiscard]] std::vector<LevelInfo> get_asks(std::size_t max_levels = 0) const;

    [[nodiscard]] std::vector<core::Order> get_all_bid_orders() const;
    [[nodiscard]] std::vector<core::Order> get_all_ask_orders() const;

    // Invariant Verification
    [[nodiscard]] bool check_invariants() const noexcept;

private:
    struct OrderLocation {
        core::Side side;
        core::PriceTicks price;
        std::list<core::Order>::iterator iterator;
    };

    std::map<core::PriceTicks, std::list<core::Order>, std::greater<core::PriceTicks>> bids_;
    std::map<core::PriceTicks, std::list<core::Order>, std::less<core::PriceTicks>> asks_;
    std::unordered_map<core::OrderId, OrderLocation> order_map_;

    core::Quantity total_bid_volume_{0};
    core::Quantity total_ask_volume_{0};
};

}  // namespace quantengine::reference
