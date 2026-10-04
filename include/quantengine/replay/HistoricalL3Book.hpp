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

// quantengine/replay/HistoricalL3Book.hpp
//
// Deterministic reconstruction of an exchange L3 (order-level) order book.
//
// DESIGN & INVARIANTS:
//   1. Maintains strict price/time (FIFO) priority per price level.
//   2. Orders are keyed by VenueOrderId (exchange reference ID), completely
//      distinct from core::OrderId (client/strategy order ID).
//   3. Supports full and partial executions (OrderExecuted), full and partial
//      cancellations (OrderCancelled), and atomic priority-reset replaces (OrderReplaced).
//   4. Provides O(1) best bid/ask queries and level depth matching reference::LevelInfo
//      for direct differential testing with ReferenceOrderBook and OptimizedOrderBook.
//   5. Exposes exact queue-ahead queries:
//      - queue_ahead_of(VenueOrderId): quantity resting ahead of a historical order.
//      - queue_ahead_at(price, side, join_seq): quantity resting ahead of a sequence number.
//   6. All prices use core::PriceTicks (int64_t fixed-point); all quantities use core::Quantity.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <list>
#include <map>
#include <optional>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "quantengine/core/trade.hpp"
#include "quantengine/core/types.hpp"
#include "quantengine/engine/canonical_state.hpp"
#include "quantengine/market/MarketEvent.hpp"
#include "quantengine/reference/reference_order_book.hpp"
#include "quantengine/replay/L3Message.hpp"

namespace quantengine::replay {

// Represents a historical resting order in the exchange order book.
struct HistoricalOrder {
    VenueOrderId venue_order_id{0};
    core::PriceTicks price{0};
    core::Quantity initial_quantity{0};
    core::Quantity remaining_quantity{0};
    core::Side side{core::Side::Buy};
    market::NanoTs entry_ts{0};
    std::uint64_t sequence_number{0};  // Monotonic sequence when inserted into the book
    market::SymbolArray symbol{};

    [[nodiscard]] constexpr bool operator==(const HistoricalOrder&) const noexcept = default;
};

class HistoricalL3Book {
public:
    HistoricalL3Book() = default;
    ~HistoricalL3Book() = default;

    HistoricalL3Book(const HistoricalL3Book&) = delete;
    HistoricalL3Book& operator=(const HistoricalL3Book&) = delete;
    HistoricalL3Book(HistoricalL3Book&&) noexcept = default;
    HistoricalL3Book& operator=(HistoricalL3Book&&) noexcept = default;

    // Dispatch any L3Message to the appropriate handler
    auto apply(const L3Message& msg) noexcept -> bool;

    // Core mutation operations
    auto apply_add(const OrderAdded& add) noexcept -> bool;
    auto apply_execute(const OrderExecuted& exec) noexcept -> bool;
    auto apply_cancel(const OrderCancelled& cancel) noexcept -> bool;
    auto apply_replace(const OrderReplaced& repl) noexcept -> bool;

    // Aggressive matching against resting historical liquidity (takes liquidity)
    auto match_aggressive(core::Side taker_side, core::Quantity quantity,
                          std::optional<core::PriceTicks> limit_price = std::nullopt)
        -> std::vector<core::Trade>;

    // Queries: BBO
    [[nodiscard]] auto best_bid_price() const noexcept -> std::optional<core::PriceTicks>;
    [[nodiscard]] auto best_ask_price() const noexcept -> std::optional<core::PriceTicks>;
    [[nodiscard]] auto best_bid_quantity() const noexcept -> std::optional<core::Quantity>;
    [[nodiscard]] auto best_ask_quantity() const noexcept -> std::optional<core::Quantity>;

    // Queries: Book totals
    [[nodiscard]] auto total_bid_volume() const noexcept -> core::Quantity;
    [[nodiscard]] auto total_ask_volume() const noexcept -> core::Quantity;
    [[nodiscard]] auto total_orders() const noexcept -> std::size_t;
    [[nodiscard]] auto is_empty() const noexcept -> bool;
    [[nodiscard]] auto current_sequence() const noexcept -> std::uint64_t;

    // Queries: Order inspection
    [[nodiscard]] auto has_order(VenueOrderId id) const noexcept -> bool;
    [[nodiscard]] auto get_order(VenueOrderId id) const noexcept -> const HistoricalOrder*;

    // Queries: Depth & Level information (returns reference::LevelInfo for equivalence testing)
    [[nodiscard]] auto bid_level_count() const noexcept -> std::size_t;
    [[nodiscard]] auto ask_level_count() const noexcept -> std::size_t;
    [[nodiscard]] auto get_bids(std::size_t max_levels = 0) const
        -> std::vector<reference::LevelInfo>;
    [[nodiscard]] auto get_asks(std::size_t max_levels = 0) const
        -> std::vector<reference::LevelInfo>;
    [[nodiscard]] auto level_quantity(core::PriceTicks price,
                                      core::Side side) const noexcept -> core::Quantity;
    [[nodiscard]] auto level_order_count(core::PriceTicks price,
                                         core::Side side) const noexcept -> std::size_t;

    // Queries: Queue position
    // Quantity of liquidity resting ahead of a historical order with the given ID
    [[nodiscard]] auto queue_ahead_of(VenueOrderId id) const noexcept -> core::Quantity;

    // Quantity of liquidity resting at (price, side) with sequence number strictly less than
    // join_seq
    [[nodiscard]] auto queue_ahead_at(core::PriceTicks price, core::Side side,
                                      std::uint64_t join_seq) const noexcept -> core::Quantity;

    // Verification & Determinism
    [[nodiscard]] auto check_invariants() const noexcept -> bool;
    [[nodiscard]] auto compute_canonical_hash() const noexcept -> std::uint64_t;

    // Reset book state
    void clear() noexcept;

private:
    struct BookLevel {
        core::PriceTicks price{0};
        core::Quantity total_quantity{0};
        std::list<HistoricalOrder> orders{};
    };

    struct OrderLocation {
        core::Side side{core::Side::Buy};
        core::PriceTicks price{0};
        std::list<HistoricalOrder>::iterator iter{};
    };

    // Bids descending (highest price first), Asks ascending (lowest price first)
    std::map<core::PriceTicks, BookLevel, std::greater<core::PriceTicks>> bids_;
    std::map<core::PriceTicks, BookLevel, std::less<core::PriceTicks>> asks_;

    std::unordered_map<VenueOrderId, OrderLocation> order_map_;

    core::Quantity total_bid_volume_{0};
    core::Quantity total_ask_volume_{0};
    std::size_t total_orders_{0};
    std::uint64_t current_sequence_{0};
};

}  // namespace quantengine::replay
