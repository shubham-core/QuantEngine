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

#include "quantengine/replay/HistoricalL3Book.hpp"

#include <algorithm>
#include <cstring>

namespace quantengine::replay {

auto HistoricalL3Book::apply(const L3Message& msg) noexcept -> bool {
    return std::visit(
        [this](const auto& m) noexcept -> bool {
            using T = std::decay_t<decltype(m)>;
            if constexpr (std::is_same_v<T, OrderAdded>) {
                return apply_add(m);
            } else if constexpr (std::is_same_v<T, OrderExecuted>) {
                return apply_execute(m);
            } else if constexpr (std::is_same_v<T, OrderCancelled>) {
                return apply_cancel(m);
            } else if constexpr (std::is_same_v<T, OrderReplaced>) {
                return apply_replace(m);
            } else if constexpr (std::is_same_v<T, TradeMessage>) {
                // Trade prints report executions/crosses, not book order mutations directly
                return true;
            }
            return false;
        },
        msg);
}

auto HistoricalL3Book::apply_add(const OrderAdded& add) noexcept -> bool {
    if (add.venue_order_id == 0 || add.price <= 0 || add.quantity == 0) {
        return false;
    }
    if (order_map_.contains(add.venue_order_id)) {
        return false;  // Duplicate venue order ID
    }

    ++current_sequence_;

    HistoricalOrder order{
        .venue_order_id = add.venue_order_id,
        .price = add.price,
        .initial_quantity = add.quantity,
        .remaining_quantity = add.quantity,
        .side = add.side,
        .entry_ts = add.exchange_ts,
        .sequence_number = current_sequence_,
        .symbol = add.symbol,
    };

    if (add.side == core::Side::Buy) {
        auto& level = bids_[add.price];
        level.price = add.price;
        level.total_quantity += add.quantity;
        level.orders.push_back(order);
        order_map_[add.venue_order_id] = OrderLocation{
            .side = core::Side::Buy,
            .price = add.price,
            .iter = std::prev(level.orders.end()),
        };
        total_bid_volume_ += add.quantity;
    } else {
        auto& level = asks_[add.price];
        level.price = add.price;
        level.total_quantity += add.quantity;
        level.orders.push_back(order);
        order_map_[add.venue_order_id] = OrderLocation{
            .side = core::Side::Sell,
            .price = add.price,
            .iter = std::prev(level.orders.end()),
        };
        total_ask_volume_ += add.quantity;
    }

    ++total_orders_;
    return true;
}

auto HistoricalL3Book::apply_execute(const OrderExecuted& exec) noexcept -> bool {
    if (exec.venue_order_id == 0 || exec.executed_qty == 0) {
        return false;
    }

    auto map_it = order_map_.find(exec.venue_order_id);
    if (map_it == order_map_.end()) {
        return false;  // Order not in book
    }

    const auto [side, price, order_it] = map_it->second;
    const core::Quantity exec_qty = std::min(exec.executed_qty, order_it->remaining_quantity);
    order_it->remaining_quantity -= exec_qty;

    if (side == core::Side::Buy) {
        auto level_it = bids_.find(price);
        level_it->second.total_quantity -= exec_qty;
        total_bid_volume_ -= exec_qty;

        if (order_it->remaining_quantity == 0) {
            level_it->second.orders.erase(order_it);
            order_map_.erase(map_it);
            --total_orders_;
            if (level_it->second.orders.empty()) {
                bids_.erase(level_it);
            }
        }
    } else {
        auto level_it = asks_.find(price);
        level_it->second.total_quantity -= exec_qty;
        total_ask_volume_ -= exec_qty;

        if (order_it->remaining_quantity == 0) {
            level_it->second.orders.erase(order_it);
            order_map_.erase(map_it);
            --total_orders_;
            if (level_it->second.orders.empty()) {
                asks_.erase(level_it);
            }
        }
    }

    return true;
}

auto HistoricalL3Book::apply_cancel(const OrderCancelled& cancel) noexcept -> bool {
    if (cancel.venue_order_id == 0 || cancel.cancelled_qty == 0) {
        return false;
    }

    auto map_it = order_map_.find(cancel.venue_order_id);
    if (map_it == order_map_.end()) {
        return false;  // Order not in book
    }

    const auto [side, price, order_it] = map_it->second;
    const core::Quantity cancel_qty = std::min(cancel.cancelled_qty, order_it->remaining_quantity);
    order_it->remaining_quantity -= cancel_qty;

    if (side == core::Side::Buy) {
        auto level_it = bids_.find(price);
        level_it->second.total_quantity -= cancel_qty;
        total_bid_volume_ -= cancel_qty;

        if (order_it->remaining_quantity == 0) {
            level_it->second.orders.erase(order_it);
            order_map_.erase(map_it);
            --total_orders_;
            if (level_it->second.orders.empty()) {
                bids_.erase(level_it);
            }
        }
    } else {
        auto level_it = asks_.find(price);
        level_it->second.total_quantity -= cancel_qty;
        total_ask_volume_ -= cancel_qty;

        if (order_it->remaining_quantity == 0) {
            level_it->second.orders.erase(order_it);
            order_map_.erase(map_it);
            --total_orders_;
            if (level_it->second.orders.empty()) {
                asks_.erase(level_it);
            }
        }
    }

    return true;
}

auto HistoricalL3Book::apply_replace(const OrderReplaced& repl) noexcept -> bool {
    if (repl.old_venue_order_id == 0 || repl.new_venue_order_id == 0 ||
        repl.old_venue_order_id == repl.new_venue_order_id || repl.new_price <= 0 ||
        repl.new_quantity == 0) {
        return false;
    }

    auto old_it = order_map_.find(repl.old_venue_order_id);
    if (old_it == order_map_.end()) {
        return false;  // Old order not found
    }
    if (order_map_.contains(repl.new_venue_order_id)) {
        return false;  // New ID collision
    }

    // Cancel old order completely
    const core::Quantity old_rem = old_it->second.iter->remaining_quantity;
    apply_cancel(OrderCancelled{
        .exchange_ts = repl.exchange_ts,
        .venue_order_id = repl.old_venue_order_id,
        .symbol = repl.symbol,
        .cancelled_qty = old_rem,
    });

    // Add new order at new terms (priority reset to tail of new level)
    return apply_add(OrderAdded{
        .exchange_ts = repl.exchange_ts,
        .venue_order_id = repl.new_venue_order_id,
        .symbol = repl.symbol,
        .price = repl.new_price,
        .quantity = repl.new_quantity,
        .side = repl.side,
    });
}

auto HistoricalL3Book::match_aggressive(core::Side taker_side, core::Quantity quantity,
                                        std::optional<core::PriceTicks> limit_price)
    -> std::vector<core::Trade> {
    std::vector<core::Trade> trades;
    if (quantity == 0)
        return trades;

    if (taker_side == core::Side::Buy) {
        while (quantity > 0 && !asks_.empty()) {
            auto level_it = asks_.begin();
            if (limit_price.has_value() && level_it->first > *limit_price) {
                break;
            }

            auto& order_list = level_it->second.orders;
            auto ord_it = order_list.begin();
            while (quantity > 0 && ord_it != order_list.end()) {
                const core::Quantity trade_qty = std::min(quantity, ord_it->remaining_quantity);
                ord_it->remaining_quantity -= trade_qty;
                level_it->second.total_quantity -= trade_qty;
                total_ask_volume_ -= trade_qty;
                quantity -= trade_qty;

                trades.push_back(core::Trade{
                    .trade_id = ++current_sequence_,
                    .maker_order_id = ord_it->venue_order_id,
                    .taker_order_id = 0,
                    .maker_side = core::Side::Sell,
                    .price = ord_it->price,
                    .quantity = trade_qty,
                    .sequence_number = current_sequence_,
                });

                if (ord_it->remaining_quantity == 0) {
                    order_map_.erase(ord_it->venue_order_id);
                    ord_it = order_list.erase(ord_it);
                    --total_orders_;
                } else {
                    ++ord_it;
                }
            }

            if (order_list.empty()) {
                asks_.erase(level_it);
            }
        }
    } else {
        while (quantity > 0 && !bids_.empty()) {
            auto level_it = bids_.begin();
            if (limit_price.has_value() && level_it->first < *limit_price) {
                break;
            }

            auto& order_list = level_it->second.orders;
            auto ord_it = order_list.begin();
            while (quantity > 0 && ord_it != order_list.end()) {
                const core::Quantity trade_qty = std::min(quantity, ord_it->remaining_quantity);
                ord_it->remaining_quantity -= trade_qty;
                level_it->second.total_quantity -= trade_qty;
                total_bid_volume_ -= trade_qty;
                quantity -= trade_qty;

                trades.push_back(core::Trade{
                    .trade_id = ++current_sequence_,
                    .maker_order_id = ord_it->venue_order_id,
                    .taker_order_id = 0,
                    .maker_side = core::Side::Buy,
                    .price = ord_it->price,
                    .quantity = trade_qty,
                    .sequence_number = current_sequence_,
                });

                if (ord_it->remaining_quantity == 0) {
                    order_map_.erase(ord_it->venue_order_id);
                    ord_it = order_list.erase(ord_it);
                    --total_orders_;
                } else {
                    ++ord_it;
                }
            }

            if (order_list.empty()) {
                bids_.erase(level_it);
            }
        }
    }

    return trades;
}

auto HistoricalL3Book::best_bid_price() const noexcept -> std::optional<core::PriceTicks> {
    if (bids_.empty())
        return std::nullopt;
    return bids_.begin()->first;
}

auto HistoricalL3Book::best_ask_price() const noexcept -> std::optional<core::PriceTicks> {
    if (asks_.empty())
        return std::nullopt;
    return asks_.begin()->first;
}

auto HistoricalL3Book::best_bid_quantity() const noexcept -> std::optional<core::Quantity> {
    if (bids_.empty())
        return std::nullopt;
    return bids_.begin()->second.total_quantity;
}

auto HistoricalL3Book::best_ask_quantity() const noexcept -> std::optional<core::Quantity> {
    if (asks_.empty())
        return std::nullopt;
    return asks_.begin()->second.total_quantity;
}

auto HistoricalL3Book::total_bid_volume() const noexcept -> core::Quantity {
    return total_bid_volume_;
}

auto HistoricalL3Book::total_ask_volume() const noexcept -> core::Quantity {
    return total_ask_volume_;
}

auto HistoricalL3Book::total_orders() const noexcept -> std::size_t {
    return total_orders_;
}

auto HistoricalL3Book::is_empty() const noexcept -> bool {
    return total_orders_ == 0;
}

auto HistoricalL3Book::current_sequence() const noexcept -> std::uint64_t {
    return current_sequence_;
}

auto HistoricalL3Book::has_order(VenueOrderId id) const noexcept -> bool {
    return order_map_.contains(id);
}

auto HistoricalL3Book::get_order(VenueOrderId id) const noexcept -> const HistoricalOrder* {
    auto it = order_map_.find(id);
    if (it == order_map_.end())
        return nullptr;
    return &(*it->second.iter);
}

auto HistoricalL3Book::bid_level_count() const noexcept -> std::size_t {
    return bids_.size();
}

auto HistoricalL3Book::ask_level_count() const noexcept -> std::size_t {
    return asks_.size();
}

auto HistoricalL3Book::get_bids(std::size_t max_levels) const -> std::vector<reference::LevelInfo> {
    std::vector<reference::LevelInfo> result;
    const std::size_t limit = (max_levels == 0) ? bids_.size() : std::min(max_levels, bids_.size());
    result.reserve(limit);

    std::size_t count = 0;
    for (const auto& [price, level] : bids_) {
        if (count >= limit)
            break;
        result.push_back(reference::LevelInfo{
            .price = price,
            .total_quantity = level.total_quantity,
            .order_count = level.orders.size(),
        });
        ++count;
    }
    return result;
}

auto HistoricalL3Book::get_asks(std::size_t max_levels) const -> std::vector<reference::LevelInfo> {
    std::vector<reference::LevelInfo> result;
    const std::size_t limit = (max_levels == 0) ? asks_.size() : std::min(max_levels, asks_.size());
    result.reserve(limit);

    std::size_t count = 0;
    for (const auto& [price, level] : asks_) {
        if (count >= limit)
            break;
        result.push_back(reference::LevelInfo{
            .price = price,
            .total_quantity = level.total_quantity,
            .order_count = level.orders.size(),
        });
        ++count;
    }
    return result;
}

auto HistoricalL3Book::level_quantity(core::PriceTicks price,
                                      core::Side side) const noexcept -> core::Quantity {
    if (side == core::Side::Buy) {
        auto it = bids_.find(price);
        return (it != bids_.end()) ? it->second.total_quantity : 0;
    }
    auto it = asks_.find(price);
    return (it != asks_.end()) ? it->second.total_quantity : 0;
}

auto HistoricalL3Book::level_order_count(core::PriceTicks price,
                                         core::Side side) const noexcept -> std::size_t {
    if (side == core::Side::Buy) {
        auto it = bids_.find(price);
        return (it != bids_.end()) ? it->second.orders.size() : 0;
    }
    auto it = asks_.find(price);
    return (it != asks_.end()) ? it->second.orders.size() : 0;
}

auto HistoricalL3Book::queue_ahead_of(VenueOrderId id) const noexcept -> core::Quantity {
    auto map_it = order_map_.find(id);
    if (map_it == order_map_.end())
        return 0;

    const auto [side, price, target_iter] = map_it->second;
    core::Quantity ahead_qty = 0;

    if (side == core::Side::Buy) {
        auto level_it = bids_.find(price);
        if (level_it != bids_.end()) {
            for (auto it = level_it->second.orders.begin(); it != target_iter; ++it) {
                ahead_qty += it->remaining_quantity;
            }
        }
    } else {
        auto level_it = asks_.find(price);
        if (level_it != asks_.end()) {
            for (auto it = level_it->second.orders.begin(); it != target_iter; ++it) {
                ahead_qty += it->remaining_quantity;
            }
        }
    }
    return ahead_qty;
}

auto HistoricalL3Book::queue_ahead_at(core::PriceTicks price, core::Side side,
                                      std::uint64_t join_seq) const noexcept -> core::Quantity {
    core::Quantity ahead_qty = 0;
    if (side == core::Side::Buy) {
        auto level_it = bids_.find(price);
        if (level_it != bids_.end()) {
            for (const auto& ord : level_it->second.orders) {
                if (ord.sequence_number < join_seq) {
                    ahead_qty += ord.remaining_quantity;
                }
            }
        }
    } else {
        auto level_it = asks_.find(price);
        if (level_it != asks_.end()) {
            for (const auto& ord : level_it->second.orders) {
                if (ord.sequence_number < join_seq) {
                    ahead_qty += ord.remaining_quantity;
                }
            }
        }
    }
    return ahead_qty;
}

auto HistoricalL3Book::check_invariants() const noexcept -> bool {
    if (order_map_.size() != total_orders_)
        return false;

    core::Quantity computed_bid_vol = 0;
    for (const auto& [price, level] : bids_) {
        core::Quantity level_vol = 0;
        for (const auto& ord : level.orders) {
            if (ord.side != core::Side::Buy || ord.price != price || ord.remaining_quantity == 0) {
                return false;
            }
            level_vol += ord.remaining_quantity;
        }
        if (level_vol != level.total_quantity || level.orders.empty() ||
            level.total_quantity == 0) {
            return false;
        }
        computed_bid_vol += level_vol;
    }
    if (computed_bid_vol != total_bid_volume_)
        return false;

    core::Quantity computed_ask_vol = 0;
    for (const auto& [price, level] : asks_) {
        core::Quantity level_vol = 0;
        for (const auto& ord : level.orders) {
            if (ord.side != core::Side::Sell || ord.price != price || ord.remaining_quantity == 0) {
                return false;
            }
            level_vol += ord.remaining_quantity;
        }
        if (level_vol != level.total_quantity || level.orders.empty() ||
            level.total_quantity == 0) {
            return false;
        }
        computed_ask_vol += level_vol;
    }
    if (computed_ask_vol != total_ask_volume_)
        return false;

    return true;
}

auto HistoricalL3Book::compute_canonical_hash() const noexcept -> std::uint64_t {
    std::vector<std::uint8_t> buffer;
    buffer.reserve(2048);

    auto append_u32 = [](std::vector<std::uint8_t>& buf, std::uint32_t val) {
        buf.push_back(static_cast<std::uint8_t>(val & 0xFF));
        buf.push_back(static_cast<std::uint8_t>((val >> 8) & 0xFF));
        buf.push_back(static_cast<std::uint8_t>((val >> 16) & 0xFF));
        buf.push_back(static_cast<std::uint8_t>((val >> 24) & 0xFF));
    };

    auto append_u64 = [](std::vector<std::uint8_t>& buf, std::uint64_t val) {
        for (int i = 0; i < 8; ++i) {
            buf.push_back(static_cast<std::uint8_t>((val >> (i * 8)) & 0xFF));
        }
    };

    auto append_i64 = [&append_u64](std::vector<std::uint8_t>& buf, std::int64_t val) {
        std::uint64_t uval;
        std::memcpy(&uval, &val, sizeof(uval));
        append_u64(buf, uval);
    };

    append_u64(buffer, current_sequence_);

    // Bids depth
    append_u32(buffer, static_cast<std::uint32_t>(bids_.size()));
    for (const auto& [price, level] : bids_) {
        append_i64(buffer, price);
        append_u64(buffer, level.total_quantity);
        append_u32(buffer, static_cast<std::uint32_t>(level.orders.size()));
    }

    // Asks depth
    append_u32(buffer, static_cast<std::uint32_t>(asks_.size()));
    for (const auto& [price, level] : asks_) {
        append_i64(buffer, price);
        append_u64(buffer, level.total_quantity);
        append_u32(buffer, static_cast<std::uint32_t>(level.orders.size()));
    }

    // Bid orders in strict FIFO order per level
    for (const auto& [price, level] : bids_) {
        for (const auto& ord : level.orders) {
            append_u64(buffer, ord.venue_order_id);
            buffer.push_back(static_cast<std::uint8_t>(ord.side));
            append_i64(buffer, ord.price);
            append_u64(buffer, ord.remaining_quantity);
            append_u64(buffer, ord.sequence_number);
        }
    }

    // Ask orders in strict FIFO order per level
    for (const auto& [price, level] : asks_) {
        for (const auto& ord : level.orders) {
            append_u64(buffer, ord.venue_order_id);
            buffer.push_back(static_cast<std::uint8_t>(ord.side));
            append_i64(buffer, ord.price);
            append_u64(buffer, ord.remaining_quantity);
            append_u64(buffer, ord.sequence_number);
        }
    }

    return engine::CanonicalState::fnv1a_64(buffer.data(), buffer.size());
}

void HistoricalL3Book::clear() noexcept {
    bids_.clear();
    asks_.clear();
    order_map_.clear();
    total_bid_volume_ = 0;
    total_ask_volume_ = 0;
    total_orders_ = 0;
    current_sequence_ = 0;
}

}  // namespace quantengine::replay
