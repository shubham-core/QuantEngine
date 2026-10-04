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

// quantengine/replay/QueuePositionTracker.hpp
//
// Manages simulated participant orders in the context of an L3 historical feed.
// Tracks queue-ahead quantity explicitly and attributes fills deterministically.

#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>

#include "quantengine/core/types.hpp"
#include "quantengine/market/MarketEvent.hpp"
#include "quantengine/replay/FifoQueueModel.hpp"
#include "quantengine/replay/HistoricalL3Book.hpp"
#include "quantengine/replay/IQueueModel.hpp"
#include "quantengine/replay/L3Message.hpp"

namespace quantengine::replay {

struct SimFillEvent {
    core::OrderId client_order_id{0};
    core::PriceTicks fill_price{0};
    core::Quantity fill_quantity{0};
    core::Side side{core::Side::Buy};
    market::NanoTs fill_ts{0};
    MatchNumber match_number{0};

    [[nodiscard]] constexpr bool operator==(const SimFillEvent&) const noexcept = default;
};

struct SimulatedOrder {
    core::OrderId client_order_id{0};
    core::Side side{core::Side::Buy};
    core::PriceTicks price{0};
    core::Quantity initial_quantity{0};
    core::Quantity remaining_quantity{0};
    core::Quantity filled_quantity{0};
    core::Quantity queue_ahead{0};   // Resting volume ahead of this order in the queue
    std::uint64_t join_sequence{0};  // Book sequence number when order was placed
    market::NanoTs entry_ts{0};
    bool active{true};

    [[nodiscard]] constexpr bool is_filled() const noexcept { return remaining_quantity == 0; }
};

class QueuePositionTracker {
public:
    // Defaults to FifoQueueModel if model is nullptr
    explicit QueuePositionTracker(std::unique_ptr<IQueueModel> model = nullptr) noexcept;

    ~QueuePositionTracker() = default;

    QueuePositionTracker(const QueuePositionTracker&) = delete;
    QueuePositionTracker& operator=(const QueuePositionTracker&) = delete;
    QueuePositionTracker(QueuePositionTracker&&) noexcept = default;
    QueuePositionTracker& operator=(QueuePositionTracker&&) noexcept = default;

    // Register a new simulated participant order.
    // Calculates initial queue ahead using the underlying book state.
    auto track_order(core::OrderId client_order_id, core::Side side, core::PriceTicks price,
                     core::Quantity quantity, const HistoricalL3Book& book,
                     market::NanoTs entry_ts) noexcept -> bool;

    // Cancel a simulated order
    auto cancel_order(core::OrderId client_order_id) noexcept -> bool;

    // Feed event hooks
    auto on_historical_execute(const OrderExecuted& exec,
                               const HistoricalOrder* order_before_exec) noexcept
        -> std::vector<SimFillEvent>;

    auto on_historical_cancel(const OrderCancelled& cancel,
                              const HistoricalOrder* order_before_cancel) noexcept -> void;

    // Inspection
    [[nodiscard]] auto has_order(core::OrderId id) const noexcept -> bool;
    [[nodiscard]] auto get_order(core::OrderId id) const noexcept -> const SimulatedOrder*;
    [[nodiscard]] auto queue_ahead_of(core::OrderId id) const noexcept -> core::Quantity;
    [[nodiscard]] auto active_order_count() const noexcept -> std::size_t;

    // Reset tracker state
    void clear() noexcept;

private:
    std::unique_ptr<IQueueModel> model_;
    std::unordered_map<core::OrderId, SimulatedOrder> orders_;
    std::size_t active_orders_{0};
};

}  // namespace quantengine::replay
