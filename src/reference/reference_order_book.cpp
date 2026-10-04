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

#include "quantengine/reference/reference_order_book.hpp"

namespace quantengine::reference {

using namespace quantengine::core;

bool ReferenceOrderBook::add_order(Order order) {
    if (!order.check_invariants()) {
        return false;
    }

    const auto id = order.order_id();
    if (order_map_.contains(id)) {
        return false;
    }

    if (order.status() == OrderStatus::New || order.status() == OrderStatus::PartiallyFilled) {
        if (!order.mark_resting()) {
            return false;
        }
    }

    const auto side = order.side();
    const auto price = order.price();
    const auto qty = order.remaining_quantity();

    if (side == Side::Buy) {
        auto& queue = bids_[price];
        queue.push_back(order);
        auto it = std::prev(queue.end());
        order_map_[id] = OrderLocation{side, price, it};
        total_bid_volume_ += qty;
    } else {
        auto& queue = asks_[price];
        queue.push_back(order);
        auto it = std::prev(queue.end());
        order_map_[id] = OrderLocation{side, price, it};
        total_ask_volume_ += qty;
    }

    return true;
}

std::optional<Order> ReferenceOrderBook::cancel_order(OrderId order_id) {
    auto map_it = order_map_.find(order_id);
    if (map_it == order_map_.end()) {
        return std::nullopt;
    }

    const auto location = map_it->second;
    auto list_it = location.iterator;
    Order cancelled_order = *list_it;

    if (location.side == Side::Buy) {
        total_bid_volume_ -= cancelled_order.remaining_quantity();
        auto& queue = bids_[location.price];
        queue.erase(list_it);
        if (queue.empty()) {
            bids_.erase(location.price);
        }
    } else {
        total_ask_volume_ -= cancelled_order.remaining_quantity();
        auto& queue = asks_[location.price];
        queue.erase(list_it);
        if (queue.empty()) {
            asks_.erase(location.price);
        }
    }

    order_map_.erase(map_it);
    [[maybe_unused]] const bool marked = cancelled_order.mark_cancelled();
    return cancelled_order;
}

bool ReferenceOrderBook::modify_order(OrderId order_id, PriceTicks new_price, Quantity new_quantity,
                                      SeqNum new_seq) {
    if (new_price <= 0 || new_quantity == 0) {
        return false;
    }

    auto map_it = order_map_.find(order_id);
    if (map_it == order_map_.end()) {
        return false;
    }

    const auto location = map_it->second;
    auto list_it = location.iterator;
    Order order = *list_it;

    // Erase from existing price queue
    if (location.side == Side::Buy) {
        total_bid_volume_ -= order.remaining_quantity();
        auto& queue = bids_[location.price];
        queue.erase(list_it);
        if (queue.empty()) {
            bids_.erase(location.price);
        }
    } else {
        total_ask_volume_ -= order.remaining_quantity();
        auto& queue = asks_[location.price];
        queue.erase(list_it);
        if (queue.empty()) {
            asks_.erase(location.price);
        }
    }

    // Reinsert with updated priority sequence and parameters
    order.update_price(new_price);
    order.update_remaining_quantity(new_quantity);
    order.update_priority_sequence(new_seq);
    if (!order.mark_resting()) {
        return false;
    }

    if (location.side == Side::Buy) {
        auto& queue = bids_[new_price];
        queue.push_back(order);
        auto it = std::prev(queue.end());
        map_it->second = OrderLocation{location.side, new_price, it};
        total_bid_volume_ += new_quantity;
    } else {
        auto& queue = asks_[new_price];
        queue.push_back(order);
        auto it = std::prev(queue.end());
        map_it->second = OrderLocation{location.side, new_price, it};
        total_ask_volume_ += new_quantity;
    }

    return true;
}

bool ReferenceOrderBook::has_order(OrderId order_id) const noexcept {
    return order_map_.contains(order_id);
}

const Order* ReferenceOrderBook::get_order(OrderId order_id) const noexcept {
    auto it = order_map_.find(order_id);
    if (it == order_map_.end()) {
        return nullptr;
    }
    return &(*it->second.iterator);
}

Order* ReferenceOrderBook::get_order_mut(OrderId order_id) noexcept {
    auto it = order_map_.find(order_id);
    if (it == order_map_.end()) {
        return nullptr;
    }
    return &(*it->second.iterator);
}

std::optional<PriceTicks> ReferenceOrderBook::best_bid_price() const noexcept {
    if (bids_.empty()) {
        return std::nullopt;
    }
    return bids_.begin()->first;
}

std::optional<PriceTicks> ReferenceOrderBook::best_ask_price() const noexcept {
    if (asks_.empty()) {
        return std::nullopt;
    }
    return asks_.begin()->first;
}

std::optional<Quantity> ReferenceOrderBook::best_bid_quantity() const noexcept {
    if (bids_.empty()) {
        return std::nullopt;
    }
    Quantity total = 0;
    for (const auto& ord : bids_.begin()->second) {
        total += ord.remaining_quantity();
    }
    return total;
}

std::optional<Quantity> ReferenceOrderBook::best_ask_quantity() const noexcept {
    if (asks_.empty()) {
        return std::nullopt;
    }
    Quantity total = 0;
    for (const auto& ord : asks_.begin()->second) {
        total += ord.remaining_quantity();
    }
    return total;
}

Order* ReferenceOrderBook::get_best_bid_order() noexcept {
    if (bids_.empty()) {
        return nullptr;
    }
    return &bids_.begin()->second.front();
}

Order* ReferenceOrderBook::get_best_ask_order() noexcept {
    if (asks_.empty()) {
        return nullptr;
    }
    return &asks_.begin()->second.front();
}

void ReferenceOrderBook::fill_best_bid_order(Quantity fill_qty) noexcept {
    if (bids_.empty() || fill_qty == 0) {
        return;
    }
    auto best_it = bids_.begin();
    auto& queue = best_it->second;
    auto& ord = queue.front();

    const Quantity actual_fill = std::min(fill_qty, ord.remaining_quantity());
    [[maybe_unused]] const bool filled = ord.apply_fill(actual_fill);
    total_bid_volume_ -= actual_fill;

    if (ord.remaining_quantity() == 0) {
        order_map_.erase(ord.order_id());
        queue.pop_front();
        if (queue.empty()) {
            bids_.erase(best_it);
        }
    }
}

void ReferenceOrderBook::fill_best_ask_order(Quantity fill_qty) noexcept {
    if (asks_.empty() || fill_qty == 0) {
        return;
    }
    auto best_it = asks_.begin();
    auto& queue = best_it->second;
    auto& ord = queue.front();

    const Quantity actual_fill = std::min(fill_qty, ord.remaining_quantity());
    [[maybe_unused]] const bool filled = ord.apply_fill(actual_fill);
    total_ask_volume_ -= actual_fill;

    if (ord.remaining_quantity() == 0) {
        order_map_.erase(ord.order_id());
        queue.pop_front();
        if (queue.empty()) {
            asks_.erase(best_it);
        }
    }
}

void ReferenceOrderBook::pop_best_bid_order() noexcept {
    if (bids_.empty()) {
        return;
    }
    fill_best_bid_order(bids_.begin()->second.front().remaining_quantity());
}

void ReferenceOrderBook::pop_best_ask_order() noexcept {
    if (asks_.empty()) {
        return;
    }
    fill_best_ask_order(asks_.begin()->second.front().remaining_quantity());
}

std::size_t ReferenceOrderBook::bid_level_count() const noexcept {
    return bids_.size();
}

std::size_t ReferenceOrderBook::ask_level_count() const noexcept {
    return asks_.size();
}

std::size_t ReferenceOrderBook::total_orders() const noexcept {
    return order_map_.size();
}

Quantity ReferenceOrderBook::total_bid_volume() const noexcept {
    return total_bid_volume_;
}

Quantity ReferenceOrderBook::total_ask_volume() const noexcept {
    return total_ask_volume_;
}

bool ReferenceOrderBook::is_empty() const noexcept {
    return order_map_.empty();
}

void ReferenceOrderBook::clear() noexcept {
    bids_.clear();
    asks_.clear();
    order_map_.clear();
    total_bid_volume_ = 0;
    total_ask_volume_ = 0;
}

std::vector<LevelInfo> ReferenceOrderBook::get_bids(std::size_t max_levels) const {
    std::vector<LevelInfo> levels;
    const auto limit = (max_levels == 0) ? bids_.size() : std::min(max_levels, bids_.size());
    levels.reserve(limit);

    std::size_t count = 0;
    for (const auto& [price, queue] : bids_) {
        if (count++ >= limit) {
            break;
        }
        Quantity qty = 0;
        for (const auto& ord : queue) {
            qty += ord.remaining_quantity();
        }
        levels.push_back(LevelInfo{price, qty, queue.size()});
    }
    return levels;
}

std::vector<LevelInfo> ReferenceOrderBook::get_asks(std::size_t max_levels) const {
    std::vector<LevelInfo> levels;
    const auto limit = (max_levels == 0) ? asks_.size() : std::min(max_levels, asks_.size());
    levels.reserve(limit);

    std::size_t count = 0;
    for (const auto& [price, queue] : asks_) {
        if (count++ >= limit) {
            break;
        }
        Quantity qty = 0;
        for (const auto& ord : queue) {
            qty += ord.remaining_quantity();
        }
        levels.push_back(LevelInfo{price, qty, queue.size()});
    }
    return levels;
}

std::vector<core::Order> ReferenceOrderBook::get_all_bid_orders() const {
    std::vector<core::Order> orders;
    orders.reserve(total_orders());
    for (const auto& [price, queue] : bids_) {
        for (const auto& ord : queue) {
            orders.push_back(ord);
        }
    }
    return orders;
}

std::vector<core::Order> ReferenceOrderBook::get_all_ask_orders() const {
    std::vector<core::Order> orders;
    orders.reserve(total_orders());
    for (const auto& [price, queue] : asks_) {
        for (const auto& ord : queue) {
            orders.push_back(ord);
        }
    }
    return orders;
}

bool ReferenceOrderBook::check_invariants() const noexcept {
    if (order_map_.size() != total_orders()) {
        return false;
    }

    Quantity calculated_bid_volume = 0;
    std::size_t calculated_bid_orders = 0;
    for (const auto& [price, queue] : bids_) {
        if (queue.empty()) {
            return false;
        }
        for (const auto& ord : queue) {
            if (ord.side() != Side::Buy || ord.price() != price) {
                return false;
            }
            if (!ord.check_invariants()) {
                return false;
            }
            calculated_bid_volume += ord.remaining_quantity();
            calculated_bid_orders++;
        }
    }

    Quantity calculated_ask_volume = 0;
    std::size_t calculated_ask_orders = 0;
    for (const auto& [price, queue] : asks_) {
        if (queue.empty()) {
            return false;
        }
        for (const auto& ord : queue) {
            if (ord.side() != Side::Sell || ord.price() != price) {
                return false;
            }
            if (!ord.check_invariants()) {
                return false;
            }
            calculated_ask_volume += ord.remaining_quantity();
            calculated_ask_orders++;
        }
    }

    if (calculated_bid_volume != total_bid_volume_) {
        return false;
    }
    if (calculated_ask_volume != total_ask_volume_) {
        return false;
    }
    if (calculated_bid_orders + calculated_ask_orders != order_map_.size()) {
        return false;
    }

    return true;
}

}  // namespace quantengine::reference
