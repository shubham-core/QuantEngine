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

#include "quantengine/replay/FifoQueueModel.hpp"

#include <algorithm>

namespace quantengine::replay {

auto FifoQueueModel::initial_queue_ahead(
    const HistoricalL3Book& book, core::PriceTicks price, core::Side side,
    [[maybe_unused]] core::Quantity quantity) noexcept -> core::Quantity {
    // Conservative baseline: joins at the absolute tail of the existing queue
    return book.level_quantity(price, side);
}

auto FifoQueueModel::on_historical_execute(
    core::Quantity& queue_ahead, core::Quantity& order_remaining_qty, core::PriceTicks order_price,
    core::Side order_side, const OrderExecuted& exec,
    const HistoricalOrder* executed_historical_order) noexcept -> FillResult {
    FillResult result{};
    if (order_remaining_qty == 0) {
        return result;
    }

    bool is_same_level = false;
    bool is_trade_through = false;

    if (executed_historical_order != nullptr) {
        if (executed_historical_order->side == order_side) {
            if (executed_historical_order->price == order_price) {
                is_same_level = true;
            } else if (order_side == core::Side::Buy &&
                       executed_historical_order->price < order_price) {
                // Aggressor swept past our bid
                is_trade_through = true;
            } else if (order_side == core::Side::Sell &&
                       executed_historical_order->price > order_price) {
                // Aggressor swept past our ask
                is_trade_through = true;
            }
        }
    }

    if (is_trade_through) {
        queue_ahead = 0;
        const core::Quantity fill_qty = std::min(order_remaining_qty, exec.executed_qty);
        order_remaining_qty -= fill_qty;
        result.fill_quantity = fill_qty;
        result.fill_price = order_price;
        result.fill_ts = exec.exchange_ts;
        result.match_number = exec.match_number;
        result.is_fill = true;
        return result;
    }

    if (is_same_level) {
        const core::Quantity consumed_ahead = std::min(queue_ahead, exec.executed_qty);
        queue_ahead -= consumed_ahead;
        const core::Quantity leftover = exec.executed_qty - consumed_ahead;

        if (queue_ahead == 0 && leftover > 0) {
            const core::Quantity fill_qty = std::min(order_remaining_qty, leftover);
            order_remaining_qty -= fill_qty;
            result.fill_quantity = fill_qty;
            result.fill_price = order_price;
            result.fill_ts = exec.exchange_ts;
            result.match_number = exec.match_number;
            result.is_fill = true;
        }
    }

    return result;
}

void FifoQueueModel::on_historical_cancel(
    core::Quantity& queue_ahead, core::PriceTicks order_price, core::Side order_side,
    std::uint64_t order_join_seq, const OrderCancelled& cancel,
    const HistoricalOrder* cancelled_historical_order) noexcept {
    if (cancelled_historical_order == nullptr)
        return;

    if (cancelled_historical_order->price == order_price &&
        cancelled_historical_order->side == order_side) {
        if (cancelled_historical_order->sequence_number <= order_join_seq) {
            // Cancelled order was entered before our order: reduce queue ahead
            const core::Quantity reduce_qty = std::min(queue_ahead, cancel.cancelled_qty);
            queue_ahead -= reduce_qty;
        }
    }
}

}  // namespace quantengine::replay
