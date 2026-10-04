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
#include <cstdint>
#include <functional>
#include <limits>
#include <map>
#include <optional>
#include <unordered_map>
#include <vector>

#include "quantengine/core/order.hpp"
#include "quantengine/core/types.hpp"
#include "quantengine/reference/reference_order_book.hpp"

namespace quantengine::optimized {

static constexpr std::uint32_t NULL_INDEX = std::numeric_limits<std::uint32_t>::max();

// Intrusive node allocated in a contiguous object pool with index-based doubly linked pointers
struct OrderNode {
    core::Order order{};
    std::uint32_t prev{NULL_INDEX};
    std::uint32_t next{NULL_INDEX};
    bool in_use{false};
};

static constexpr std::size_t kDefaultCapacity = 65536;

// Contiguous object pool with free list recycling to eliminate per-order heap allocations
class OrderPool {
public:
    explicit OrderPool(std::size_t initial_capacity = kDefaultCapacity);

    [[nodiscard]] std::uint32_t allocate(core::Order order);
    void deallocate(std::uint32_t index) noexcept;

    [[nodiscard]] OrderNode& operator[](std::uint32_t index) noexcept { return nodes_[index]; }
    [[nodiscard]] const OrderNode& operator[](std::uint32_t index) const noexcept {
        return nodes_[index];
    }

    [[nodiscard]] std::size_t capacity() const noexcept { return nodes_.size(); }
    [[nodiscard]] std::size_t active_count() const noexcept { return active_count_; }

    void clear() noexcept;

private:
    std::vector<OrderNode> nodes_;
    std::vector<std::uint32_t> free_list_;
    std::size_t active_count_{0};
};

struct PriceLevel {
    core::PriceTicks price{0};
    core::Quantity total_quantity{0};
    std::uint32_t order_count{0};
    std::uint32_t head{NULL_INDEX};
    std::uint32_t tail{NULL_INDEX};
};

class OptimizedOrderBook {
public:
    explicit OptimizedOrderBook(std::size_t initial_capacity = kDefaultCapacity);
    ~OptimizedOrderBook() = default;

    OptimizedOrderBook(const OptimizedOrderBook&) = delete;
    OptimizedOrderBook& operator=(const OptimizedOrderBook&) = delete;
    OptimizedOrderBook(OptimizedOrderBook&&) noexcept = default;
    OptimizedOrderBook& operator=(OptimizedOrderBook&&) noexcept = default;

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

    [[nodiscard]] std::vector<reference::LevelInfo> get_bids(std::size_t max_levels = 0) const;
    [[nodiscard]] std::vector<reference::LevelInfo> get_asks(std::size_t max_levels = 0) const;

    [[nodiscard]] std::vector<core::Order> get_all_bid_orders() const;
    [[nodiscard]] std::vector<core::Order> get_all_ask_orders() const;

    // Invariant Verification
    [[nodiscard]] bool check_invariants() const noexcept;

    // Pool statistics
    [[nodiscard]] std::size_t pool_capacity() const noexcept { return pool_.capacity(); }

private:
    void unlink_order_from_level(PriceLevel& level, std::uint32_t node_idx) noexcept;
    void append_order_to_level(PriceLevel& level, std::uint32_t node_idx) noexcept;

    OrderPool pool_;

    // Maps OrderId -> Pool Index
    std::unordered_map<core::OrderId, std::uint32_t> order_index_;

    // Sorted Price Levels
    std::map<core::PriceTicks, PriceLevel, std::greater<core::PriceTicks>> bids_;
    std::map<core::PriceTicks, PriceLevel, std::less<core::PriceTicks>> asks_;

    core::Quantity total_bid_volume_{0};
    core::Quantity total_ask_volume_{0};
};

// Layout guard: OrderNode must remain compact for pool cache density.
// Current layout on x86-64 (GCC 13, Order=56B, indices=4B each, bool=1B):
//   Order(56) | prev(4) | next(4) | in_use(1) + 7pad = 72 bytes
//   => 0.89 nodes per 64-byte cache line
// To improve density: shrink Quantity to uint32_t (saves 16B on Order -> 40B -> OrderNode 56B,
// 1.14/CL) or add alignas(64) and pad to full cache line to eliminate false sharing.
static_assert(sizeof(OrderNode) == 72,
              "OrderNode layout changed — update cache-line density docs in ARCHITECTURE.md");

// ALLOCATION CONTRACT:
//   - Order node allocation/deallocation: O(1), zero heap (OrderPool free-list).
//   - Price level insertion (new PriceTicks): O(log M) + std::map node heap allocation.
//   - Price level removal (empty level erase): O(log M) + std::map node heap deallocation.
//   - order_index_ lookup/erase: O(1) avg, std::unordered_map may rehash on growth.
//   For a deep book with many active price levels, price-level churn is the remaining
//   allocation source. A flat sorted array or preallocated node pool for map nodes would
//   eliminate it; current design prioritises simplicity over that last allocation class.

}  // namespace quantengine::optimized
