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

#include "quantengine/optimized/optimized_order_book.hpp"

#include <algorithm>

namespace quantengine::optimized {

using namespace quantengine::core;

OrderPool::OrderPool(std::size_t initial_capacity) {
    nodes_.resize(initial_capacity);
    free_list_.reserve(initial_capacity);
    for (std::size_t i = initial_capacity; i > 0; --i) {
        free_list_.push_back(static_cast<std::uint32_t>(i - 1));
    }
}

std::uint32_t OrderPool::allocate(Order order) {
    if (free_list_.empty()) {
        const std::size_t old_size = nodes_.size();
        const std::size_t new_size = old_size * 2;
        nodes_.resize(new_size);
        free_list_.reserve(old_size);
        for (std::size_t i = new_size; i > old_size; --i) {
            free_list_.push_back(static_cast<std::uint32_t>(i - 1));
        }
    }

    const std::uint32_t index = free_list_.back();
    free_list_.pop_back();

    nodes_[index] =
        OrderNode{.order = order, .prev = NULL_INDEX, .next = NULL_INDEX, .in_use = true};
    active_count_++;
    return index;
}

void OrderPool::deallocate(std::uint32_t index) noexcept {
    nodes_[index].in_use = false;
    nodes_[index].prev = NULL_INDEX;
    nodes_[index].next = NULL_INDEX;
    free_list_.push_back(index);
    active_count_--;
}

void OrderPool::clear() noexcept {
    active_count_ = 0;
    free_list_.clear();
    const std::size_t n = nodes_.size();
    for (std::size_t i = n; i > 0; --i) {
        const auto idx = static_cast<std::uint32_t>(i - 1);
        nodes_[idx].in_use = false;
        free_list_.push_back(idx);
    }
}

OptimizedOrderBook::OptimizedOrderBook(std::size_t initial_capacity) : pool_(initial_capacity) {}

void OptimizedOrderBook::append_order_to_level(PriceLevel& level, std::uint32_t node_idx) noexcept {
    auto& node = pool_[node_idx];
    node.next = NULL_INDEX;
    node.prev = level.tail;

    if (level.tail != NULL_INDEX) {
        pool_[level.tail].next = node_idx;
    } else {
        level.head = node_idx;
    }
    level.tail = node_idx;
    level.order_count++;
    level.total_quantity += node.order.remaining_quantity();
}

void OptimizedOrderBook::unlink_order_from_level(PriceLevel& level,
                                                 std::uint32_t node_idx) noexcept {
    auto& node = pool_[node_idx];
    const auto prev_idx = node.prev;
    const auto next_idx = node.next;

    if (prev_idx != NULL_INDEX) {
        pool_[prev_idx].next = next_idx;
    } else {
        level.head = next_idx;
    }

    if (next_idx != NULL_INDEX) {
        pool_[next_idx].prev = prev_idx;
    } else {
        level.tail = prev_idx;
    }

    level.order_count--;
    level.total_quantity -= node.order.remaining_quantity();

    node.prev = NULL_INDEX;
    node.next = NULL_INDEX;
}

bool OptimizedOrderBook::add_order(Order order) {
    if (!order.check_invariants()) {
        return false;
    }

    const auto id = order.order_id();
    if (order_index_.contains(id)) {
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

    const auto node_idx = pool_.allocate(order);

    if (side == Side::Buy) {
        auto& level = bids_[price];
        level.price = price;
        append_order_to_level(level, node_idx);
        total_bid_volume_ += qty;
    } else {
        auto& level = asks_[price];
        level.price = price;
        append_order_to_level(level, node_idx);
        total_ask_volume_ += qty;
    }

    order_index_[id] = node_idx;
    return true;
}

std::optional<Order> OptimizedOrderBook::cancel_order(OrderId order_id) {
    auto it = order_index_.find(order_id);
    if (it == order_index_.end()) {
        return std::nullopt;
    }

    const auto node_idx = it->second;
    auto& node = pool_[node_idx];
    Order cancelled_order = node.order;

    if (cancelled_order.side() == Side::Buy) {
        total_bid_volume_ -= cancelled_order.remaining_quantity();
        auto& level = bids_[cancelled_order.price()];
        unlink_order_from_level(level, node_idx);
        if (level.order_count == 0) {
            bids_.erase(cancelled_order.price());
        }
    } else {
        total_ask_volume_ -= cancelled_order.remaining_quantity();
        auto& level = asks_[cancelled_order.price()];
        unlink_order_from_level(level, node_idx);
        if (level.order_count == 0) {
            asks_.erase(cancelled_order.price());
        }
    }

    pool_.deallocate(node_idx);
    order_index_.erase(it);
    [[maybe_unused]] const bool marked = cancelled_order.mark_cancelled();
    return cancelled_order;
}

bool OptimizedOrderBook::modify_order(OrderId order_id, PriceTicks new_price, Quantity new_quantity,
                                      SeqNum new_seq) {
    if (new_price <= 0 || new_quantity == 0) {
        return false;
    }

    auto it = order_index_.find(order_id);
    if (it == order_index_.end()) {
        return false;
    }

    const auto node_idx = it->second;
    auto& node = pool_[node_idx];
    const auto side = node.order.side();
    const auto old_price = node.order.price();

    // Unlink from old level
    if (side == Side::Buy) {
        total_bid_volume_ -= node.order.remaining_quantity();
        auto& level = bids_[old_price];
        unlink_order_from_level(level, node_idx);
        if (level.order_count == 0) {
            bids_.erase(old_price);
        }
    } else {
        total_ask_volume_ -= node.order.remaining_quantity();
        auto& level = asks_[old_price];
        unlink_order_from_level(level, node_idx);
        if (level.order_count == 0) {
            asks_.erase(old_price);
        }
    }

    // Update order fields
    node.order.update_price(new_price);
    node.order.update_remaining_quantity(new_quantity);
    node.order.update_priority_sequence(new_seq);
    if (!node.order.mark_resting()) {
        pool_.deallocate(node_idx);
        order_index_.erase(it);
        return false;
    }

    // Re-append to tail of new level
    if (side == Side::Buy) {
        auto& level = bids_[new_price];
        level.price = new_price;
        append_order_to_level(level, node_idx);
        total_bid_volume_ += new_quantity;
    } else {
        auto& level = asks_[new_price];
        level.price = new_price;
        append_order_to_level(level, node_idx);
        total_ask_volume_ += new_quantity;
    }

    return true;
}

bool OptimizedOrderBook::has_order(OrderId order_id) const noexcept {
    return order_index_.contains(order_id);
}

const Order* OptimizedOrderBook::get_order(OrderId order_id) const noexcept {
    auto it = order_index_.find(order_id);
    if (it == order_index_.end()) {
        return nullptr;
    }
    return &pool_[it->second].order;
}

Order* OptimizedOrderBook::get_order_mut(OrderId order_id) noexcept {
    auto it = order_index_.find(order_id);
    if (it == order_index_.end()) {
        return nullptr;
    }
    return &pool_[it->second].order;
}

std::optional<PriceTicks> OptimizedOrderBook::best_bid_price() const noexcept {
    if (bids_.empty()) {
        return std::nullopt;
    }
    return bids_.begin()->first;
}

std::optional<PriceTicks> OptimizedOrderBook::best_ask_price() const noexcept {
    if (asks_.empty()) {
        return std::nullopt;
    }
    return asks_.begin()->first;
}

std::optional<Quantity> OptimizedOrderBook::best_bid_quantity() const noexcept {
    if (bids_.empty()) {
        return std::nullopt;
    }
    return bids_.begin()->second.total_quantity;
}

std::optional<Quantity> OptimizedOrderBook::best_ask_quantity() const noexcept {
    if (asks_.empty()) {
        return std::nullopt;
    }
    return asks_.begin()->second.total_quantity;
}

Order* OptimizedOrderBook::get_best_bid_order() noexcept {
    if (bids_.empty()) {
        return nullptr;
    }
    const auto head_idx = bids_.begin()->second.head;
    if (head_idx == NULL_INDEX) {
        return nullptr;
    }
    return &pool_[head_idx].order;
}

Order* OptimizedOrderBook::get_best_ask_order() noexcept {
    if (asks_.empty()) {
        return nullptr;
    }
    const auto head_idx = asks_.begin()->second.head;
    if (head_idx == NULL_INDEX) {
        return nullptr;
    }
    return &pool_[head_idx].order;
}

void OptimizedOrderBook::fill_best_bid_order(Quantity fill_qty) noexcept {
    if (bids_.empty() || fill_qty == 0) {
        return;
    }
    auto best_it = bids_.begin();
    auto& level = best_it->second;
    const auto head_idx = level.head;
    if (head_idx == NULL_INDEX) {
        return;
    }

    auto& node = pool_[head_idx];
    const Quantity actual_fill = std::min(fill_qty, node.order.remaining_quantity());
    [[maybe_unused]] const bool filled = node.order.apply_fill(actual_fill);

    level.total_quantity -= actual_fill;
    total_bid_volume_ -= actual_fill;

    if (node.order.remaining_quantity() == 0) {
        order_index_.erase(node.order.order_id());
        unlink_order_from_level(level, head_idx);
        pool_.deallocate(head_idx);

        if (level.order_count == 0) {
            bids_.erase(best_it);
        }
    }
}

void OptimizedOrderBook::fill_best_ask_order(Quantity fill_qty) noexcept {
    if (asks_.empty() || fill_qty == 0) {
        return;
    }
    auto best_it = asks_.begin();
    auto& level = best_it->second;
    const auto head_idx = level.head;
    if (head_idx == NULL_INDEX) {
        return;
    }

    auto& node = pool_[head_idx];
    const Quantity actual_fill = std::min(fill_qty, node.order.remaining_quantity());
    [[maybe_unused]] const bool filled = node.order.apply_fill(actual_fill);

    level.total_quantity -= actual_fill;
    total_ask_volume_ -= actual_fill;

    if (node.order.remaining_quantity() == 0) {
        order_index_.erase(node.order.order_id());
        unlink_order_from_level(level, head_idx);
        pool_.deallocate(head_idx);

        if (level.order_count == 0) {
            asks_.erase(best_it);
        }
    }
}

void OptimizedOrderBook::pop_best_bid_order() noexcept {
    if (bids_.empty()) {
        return;
    }
    const auto head_idx = bids_.begin()->second.head;
    if (head_idx != NULL_INDEX) {
        fill_best_bid_order(pool_[head_idx].order.remaining_quantity());
    }
}

void OptimizedOrderBook::pop_best_ask_order() noexcept {
    if (asks_.empty()) {
        return;
    }
    const auto head_idx = asks_.begin()->second.head;
    if (head_idx != NULL_INDEX) {
        fill_best_ask_order(pool_[head_idx].order.remaining_quantity());
    }
}

std::size_t OptimizedOrderBook::bid_level_count() const noexcept {
    return bids_.size();
}

std::size_t OptimizedOrderBook::ask_level_count() const noexcept {
    return asks_.size();
}

std::size_t OptimizedOrderBook::total_orders() const noexcept {
    return order_index_.size();
}

Quantity OptimizedOrderBook::total_bid_volume() const noexcept {
    return total_bid_volume_;
}

Quantity OptimizedOrderBook::total_ask_volume() const noexcept {
    return total_ask_volume_;
}

bool OptimizedOrderBook::is_empty() const noexcept {
    return order_index_.empty();
}

void OptimizedOrderBook::clear() noexcept {
    pool_.clear();
    order_index_.clear();
    bids_.clear();
    asks_.clear();
    total_bid_volume_ = 0;
    total_ask_volume_ = 0;
}

std::vector<reference::LevelInfo> OptimizedOrderBook::get_bids(std::size_t max_levels) const {
    std::vector<reference::LevelInfo> levels;
    const auto limit = (max_levels == 0) ? bids_.size() : std::min(max_levels, bids_.size());
    levels.reserve(limit);

    std::size_t count = 0;
    for (const auto& [price, level] : bids_) {
        if (count++ >= limit) {
            break;
        }
        levels.push_back(reference::LevelInfo{price, level.total_quantity, level.order_count});
    }
    return levels;
}

std::vector<reference::LevelInfo> OptimizedOrderBook::get_asks(std::size_t max_levels) const {
    std::vector<reference::LevelInfo> levels;
    const auto limit = (max_levels == 0) ? asks_.size() : std::min(max_levels, asks_.size());
    levels.reserve(limit);

    std::size_t count = 0;
    for (const auto& [price, level] : asks_) {
        if (count++ >= limit) {
            break;
        }
        levels.push_back(reference::LevelInfo{price, level.total_quantity, level.order_count});
    }
    return levels;
}

std::vector<core::Order> OptimizedOrderBook::get_all_bid_orders() const {
    std::vector<core::Order> orders;
    orders.reserve(total_orders());
    for (const auto& [price, level] : bids_) {
        std::uint32_t curr = level.head;
        while (curr != NULL_INDEX) {
            orders.push_back(pool_[curr].order);
            curr = pool_[curr].next;
        }
    }
    return orders;
}

std::vector<core::Order> OptimizedOrderBook::get_all_ask_orders() const {
    std::vector<core::Order> orders;
    orders.reserve(total_orders());
    for (const auto& [price, level] : asks_) {
        std::uint32_t curr = level.head;
        while (curr != NULL_INDEX) {
            orders.push_back(pool_[curr].order);
            curr = pool_[curr].next;
        }
    }
    return orders;
}

bool OptimizedOrderBook::check_invariants() const noexcept {
    if (order_index_.size() != pool_.active_count()) {
        return false;
    }

    Quantity calculated_bid_volume = 0;
    std::size_t calculated_bid_orders = 0;
    for (const auto& [price, level] : bids_) {
        if (level.order_count == 0 || level.head == NULL_INDEX || level.tail == NULL_INDEX) {
            return false;
        }

        Quantity level_vol = 0;
        std::size_t count = 0;
        std::uint32_t curr = level.head;
        std::uint32_t prev = NULL_INDEX;

        while (curr != NULL_INDEX) {
            const auto& node = pool_[curr];
            if (!node.in_use || node.order.side() != Side::Buy || node.order.price() != price) {
                return false;
            }
            if (node.prev != prev) {
                return false;
            }
            if (!node.order.check_invariants()) {
                return false;
            }

            level_vol += node.order.remaining_quantity();
            count++;
            prev = curr;
            curr = node.next;
        }

        if (prev != level.tail || count != level.order_count || level_vol != level.total_quantity) {
            return false;
        }

        calculated_bid_volume += level.total_quantity;
        calculated_bid_orders += level.order_count;
    }

    Quantity calculated_ask_volume = 0;
    std::size_t calculated_ask_orders = 0;
    for (const auto& [price, level] : asks_) {
        if (level.order_count == 0 || level.head == NULL_INDEX || level.tail == NULL_INDEX) {
            return false;
        }

        Quantity level_vol = 0;
        std::size_t count = 0;
        std::uint32_t curr = level.head;
        std::uint32_t prev = NULL_INDEX;

        while (curr != NULL_INDEX) {
            const auto& node = pool_[curr];
            if (!node.in_use || node.order.side() != Side::Sell || node.order.price() != price) {
                return false;
            }
            if (node.prev != prev) {
                return false;
            }
            if (!node.order.check_invariants()) {
                return false;
            }

            level_vol += node.order.remaining_quantity();
            count++;
            prev = curr;
            curr = node.next;
        }

        if (prev != level.tail || count != level.order_count || level_vol != level.total_quantity) {
            return false;
        }

        calculated_ask_volume += level.total_quantity;
        calculated_ask_orders += level.order_count;
    }

    if (calculated_bid_volume != total_bid_volume_) {
        return false;
    }
    if (calculated_ask_volume != total_ask_volume_) {
        return false;
    }
    if (calculated_bid_orders + calculated_ask_orders != order_index_.size()) {
        return false;
    }

    return true;
}

}  // namespace quantengine::optimized
