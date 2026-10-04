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

#include "quantengine/replay/QueuePositionTracker.hpp"

#include <algorithm>

namespace quantengine::replay {

QueuePositionTracker::QueuePositionTracker(std::unique_ptr<IQueueModel> model) noexcept
    : model_(std::move(model)) {
    if (!model_) {
        model_ = std::make_unique<FifoQueueModel>();
    }
}

auto QueuePositionTracker::track_order(core::OrderId client_order_id, core::Side side,
                                       core::PriceTicks price, core::Quantity quantity,
                                       const HistoricalL3Book& book,
                                       market::NanoTs entry_ts) noexcept -> bool {
    if (client_order_id == 0 || price <= 0 || quantity == 0) {
        return false;
    }
    if (orders_.contains(client_order_id)) {
        return false;  // Order ID collision
    }

    core::Quantity ahead = model_->initial_queue_ahead(book, price, side, quantity);

    // Account for prior active simulated orders resting at the exact same price and side
    for (const auto& [id, ord] : orders_) {
        if (ord.active && ord.side == side && ord.price == price) {
            ahead += ord.remaining_quantity;
        }
    }

    const std::uint64_t seq = book.current_sequence();

    orders_.emplace(client_order_id, SimulatedOrder{
                                         .client_order_id = client_order_id,
                                         .side = side,
                                         .price = price,
                                         .initial_quantity = quantity,
                                         .remaining_quantity = quantity,
                                         .filled_quantity = 0,
                                         .queue_ahead = ahead,
                                         .join_sequence = seq,
                                         .entry_ts = entry_ts,
                                         .active = true,
                                     });

    ++active_orders_;
    return true;
}

auto QueuePositionTracker::cancel_order(core::OrderId client_order_id) noexcept -> bool {
    auto it = orders_.find(client_order_id);
    if (it == orders_.end() || !it->second.active) {
        return false;
    }
    it->second.active = false;
    --active_orders_;
    return true;
}

auto QueuePositionTracker::on_historical_execute(const OrderExecuted& exec,
                                                 const HistoricalOrder* order_before_exec) noexcept
    -> std::vector<SimFillEvent> {
    std::vector<SimFillEvent> fills;
    if (active_orders_ == 0 || exec.executed_qty == 0) {
        return fills;
    }

    // Collect active orders to evaluate
    std::vector<SimulatedOrder*> candidates;
    candidates.reserve(orders_.size());
    for (auto& [id, ord] : orders_) {
        if (ord.active) {
            candidates.push_back(&ord);
        }
    }

    // Sort by price/time priority:
    // Bids: highest price first; at same price: earliest entry/join sequence
    // Asks: lowest price first; at same price: earliest entry/join sequence
    std::sort(candidates.begin(), candidates.end(),
              [](const SimulatedOrder* a, const SimulatedOrder* b) {
                  if (a->side != b->side) {
                      return a->side == core::Side::Buy;
                  }
                  if (a->side == core::Side::Buy) {
                      if (a->price != b->price)
                          return a->price > b->price;
                  } else {
                      if (a->price != b->price)
                          return a->price < b->price;
                  }
                  if (a->join_sequence != b->join_sequence) {
                      return a->join_sequence < b->join_sequence;
                  }
                  return a->client_order_id < b->client_order_id;
              });

    core::Quantity available_fill_qty = exec.executed_qty;

    for (auto* ord : candidates) {
        if (!ord->active)
            continue;

        // Check if execution is relevant to this order (same side, price matching or crossing)
        if (order_before_exec != nullptr && order_before_exec->side == ord->side) {
            bool is_trade_through = false;
            bool is_same_level = (order_before_exec->price == ord->price);

            if (ord->side == core::Side::Buy && order_before_exec->price < ord->price) {
                is_trade_through = true;
            } else if (ord->side == core::Side::Sell && order_before_exec->price > ord->price) {
                is_trade_through = true;
            }

            if (is_trade_through) {
                ord->queue_ahead = 0;
                const core::Quantity fill_qty =
                    std::min(ord->remaining_quantity, available_fill_qty);
                if (fill_qty > 0) {
                    ord->remaining_quantity -= fill_qty;
                    ord->filled_quantity += fill_qty;
                    available_fill_qty -= fill_qty;
                    fills.push_back(SimFillEvent{
                        .client_order_id = ord->client_order_id,
                        .fill_price = ord->price,
                        .fill_quantity = fill_qty,
                        .side = ord->side,
                        .fill_ts = exec.exchange_ts,
                        .match_number = exec.match_number,
                    });
                    if (ord->is_filled()) {
                        ord->active = false;
                        --active_orders_;
                    }
                }
            } else if (is_same_level) {
                const core::Quantity consumed_ahead = std::min(ord->queue_ahead, exec.executed_qty);
                ord->queue_ahead -= consumed_ahead;

                if (ord->queue_ahead == 0 && available_fill_qty > 0) {
                    // How much remaining execution volume reached this position in the queue
                    core::Quantity leftover = 0;
                    if (exec.executed_qty > consumed_ahead) {
                        leftover = std::min(exec.executed_qty - consumed_ahead, available_fill_qty);
                    }
                    if (leftover > 0 && ord->remaining_quantity > 0) {
                        const core::Quantity fill_qty = std::min(ord->remaining_quantity, leftover);
                        ord->remaining_quantity -= fill_qty;
                        ord->filled_quantity += fill_qty;
                        available_fill_qty -= fill_qty;
                        fills.push_back(SimFillEvent{
                            .client_order_id = ord->client_order_id,
                            .fill_price = ord->price,
                            .fill_quantity = fill_qty,
                            .side = ord->side,
                            .fill_ts = exec.exchange_ts,
                            .match_number = exec.match_number,
                        });
                        if (ord->is_filled()) {
                            ord->active = false;
                            --active_orders_;
                        }
                    }
                }
            }
        }
    }

    return fills;
}

auto QueuePositionTracker::on_historical_cancel(
    const OrderCancelled& cancel, const HistoricalOrder* order_before_cancel) noexcept -> void {
    for (auto& [id, ord] : orders_) {
        if (!ord.active)
            continue;

        model_->on_historical_cancel(ord.queue_ahead, ord.price, ord.side, ord.join_sequence,
                                     cancel, order_before_cancel);
    }
}

auto QueuePositionTracker::has_order(core::OrderId id) const noexcept -> bool {
    return orders_.contains(id);
}

auto QueuePositionTracker::get_order(core::OrderId id) const noexcept -> const SimulatedOrder* {
    auto it = orders_.find(id);
    if (it == orders_.end())
        return nullptr;
    return &it->second;
}

auto QueuePositionTracker::queue_ahead_of(core::OrderId id) const noexcept -> core::Quantity {
    auto it = orders_.find(id);
    if (it == orders_.end())
        return 0;
    return it->second.queue_ahead;
}

auto QueuePositionTracker::active_order_count() const noexcept -> std::size_t {
    return active_orders_;
}

void QueuePositionTracker::clear() noexcept {
    orders_.clear();
    active_orders_ = 0;
}

}  // namespace quantengine::replay
