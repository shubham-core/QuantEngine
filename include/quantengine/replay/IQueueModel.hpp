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

// quantengine/replay/IQueueModel.hpp
//
// Abstract interface for queue-position and fill attribution models.
//
// In an L3 simulation, simulated participant orders sit alongside historical
// orders at the same price level. The queue model governs:
//   1. How initial queue position is assigned when an order enters.
//   2. How historical order cancellations affect queue priority.
//   3. How historical executions deplete the queue ahead and trigger fills.

#include <cstdint>
#include <string_view>

#include "quantengine/core/types.hpp"
#include "quantengine/market/MarketEvent.hpp"
#include "quantengine/replay/HistoricalL3Book.hpp"
#include "quantengine/replay/L3Message.hpp"

namespace quantengine::replay {

struct FillResult {
    core::Quantity fill_quantity{0};
    core::PriceTicks fill_price{0};
    market::NanoTs fill_ts{0};
    MatchNumber match_number{0};
    bool is_fill{false};

    [[nodiscard]] constexpr bool operator==(const FillResult&) const noexcept = default;
};

class IQueueModel {
public:
    virtual ~IQueueModel() = default;

    // Determine initial volume resting ahead of a new simulated order
    [[nodiscard]] virtual auto initial_queue_ahead(
        const HistoricalL3Book& book, core::PriceTicks price, core::Side side,
        core::Quantity quantity) noexcept -> core::Quantity = 0;

    // Evaluates a historical execution against a resting simulated order.
    // Mutates queue_ahead and order_remaining_qty; returns fill details if any.
    virtual auto on_historical_execute(
        core::Quantity& queue_ahead, core::Quantity& order_remaining_qty,
        core::PriceTicks order_price, core::Side order_side, const OrderExecuted& exec,
        const HistoricalOrder* executed_historical_order) noexcept -> FillResult = 0;

    // Evaluates a historical cancellation against a resting simulated order.
    // Mutates queue_ahead if the cancelled order was ahead in queue.
    virtual void on_historical_cancel(
        core::Quantity& queue_ahead, core::PriceTicks order_price, core::Side order_side,
        std::uint64_t order_join_seq, const OrderCancelled& cancel,
        const HistoricalOrder* cancelled_historical_order) noexcept = 0;

    [[nodiscard]] virtual auto model_name() const noexcept -> std::string_view = 0;
};

}  // namespace quantengine::replay
