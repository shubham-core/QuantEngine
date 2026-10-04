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

#include <algorithm>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "quantengine/core/events.hpp"
#include "quantengine/core/order.hpp"
#include "quantengine/core/trade.hpp"
#include "quantengine/core/types.hpp"
#include "quantengine/optimized/optimized_order_book.hpp"
#include "quantengine/reference/reference_order_book.hpp"

namespace quantengine::engine {

template <typename BookType>
class GenericMatchingEngine {
public:
    GenericMatchingEngine() = default;
    explicit GenericMatchingEngine(BookType book) : book_(std::move(book)) {}

    // Event Command Processor
    [[nodiscard]] core::ExecutionReport process_command(const core::OrderCommand& command) {
        core::SeqNum seq = command.sequence_number;
        if (seq == 0) {
            seq = next_sequence_++;
        } else if (seq >= next_sequence_) {
            next_sequence_ = seq + 1;
        }

        return std::visit(
            [this, seq](const auto& payload) -> core::ExecutionReport {
                using T = std::decay_t<decltype(payload)>;
                if constexpr (std::is_same_v<T, core::CreateOrderCommand>) {
                    return handle_create(seq, payload);
                } else if constexpr (std::is_same_v<T, core::CancelOrderCommand>) {
                    return handle_cancel(seq, payload);
                } else if constexpr (std::is_same_v<T, core::ModifyOrderCommand>) {
                    return handle_modify(seq, payload);
                }
            },
            command.payload);
    }

    // Direct Convenience API
    [[nodiscard]] core::ExecutionReport submit_order(core::OrderId order_id, core::Side side,
                                                     core::PriceTicks price,
                                                     core::Quantity quantity) {
        const core::SeqNum seq = next_sequence_++;
        return handle_create(seq, core::CreateOrderCommand{order_id, side, price, quantity});
    }

    [[nodiscard]] core::ExecutionReport cancel_order(core::OrderId order_id) {
        const core::SeqNum seq = next_sequence_++;
        return handle_cancel(seq, core::CancelOrderCommand{order_id});
    }

    [[nodiscard]] core::ExecutionReport modify_order(core::OrderId order_id,
                                                     core::PriceTicks new_price,
                                                     core::Quantity new_quantity) {
        const core::SeqNum seq = next_sequence_++;
        return handle_modify(seq, core::ModifyOrderCommand{order_id, new_price, new_quantity});
    }

    // Counters
    [[nodiscard]] core::SeqNum current_sequence() const noexcept {
        return (next_sequence_ > 1) ? next_sequence_ - 1 : 0;
    }
    [[nodiscard]] core::TradeId current_trade_id() const noexcept {
        return (next_trade_id_ > 1) ? next_trade_id_ - 1 : 0;
    }

    // Book Access
    [[nodiscard]] const BookType& book() const noexcept { return book_; }
    [[nodiscard]] BookType& book() noexcept { return book_; }

    void reset() noexcept {
        book_.clear();
        next_sequence_ = 1;
        next_trade_id_ = 1;
    }

private:
    core::ExecutionReport handle_create(core::SeqNum seq, const core::CreateOrderCommand& cmd) {
        if (cmd.order_id == 0) {
            return core::ExecutionReport{.order_id = cmd.order_id,
                                         .status = core::OrderStatus::Rejected,
                                         .reject_reason = core::RejectReason::UnknownOrder,
                                         .remaining_quantity = 0,
                                         .filled_quantity = 0,
                                         .price = cmd.price,
                                         .side = cmd.side,
                                         .sequence_number = seq,
                                         .trades = {}};
        }

        if (cmd.price <= 0) {
            return core::ExecutionReport{.order_id = cmd.order_id,
                                         .status = core::OrderStatus::Rejected,
                                         .reject_reason = core::RejectReason::InvalidPrice,
                                         .remaining_quantity = 0,
                                         .filled_quantity = 0,
                                         .price = cmd.price,
                                         .side = cmd.side,
                                         .sequence_number = seq,
                                         .trades = {}};
        }

        if (cmd.quantity == 0) {
            return core::ExecutionReport{.order_id = cmd.order_id,
                                         .status = core::OrderStatus::Rejected,
                                         .reject_reason = core::RejectReason::InvalidQuantity,
                                         .remaining_quantity = 0,
                                         .filled_quantity = 0,
                                         .price = cmd.price,
                                         .side = cmd.side,
                                         .sequence_number = seq,
                                         .trades = {}};
        }

        if (book_.has_order(cmd.order_id)) {
            return core::ExecutionReport{.order_id = cmd.order_id,
                                         .status = core::OrderStatus::Rejected,
                                         .reject_reason = core::RejectReason::DuplicateOrderId,
                                         .remaining_quantity = 0,
                                         .filled_quantity = 0,
                                         .price = cmd.price,
                                         .side = cmd.side,
                                         .sequence_number = seq,
                                         .trades = {}};
        }

        core::Order incoming(cmd.order_id, cmd.side, cmd.price, cmd.quantity, seq);
        std::vector<core::Trade> trades;

        if (cmd.side == core::Side::Buy) {
            while (incoming.remaining_quantity() > 0) {
                const auto best_ask = book_.best_ask_price();
                if (!best_ask || incoming.price() < *best_ask) {
                    break;
                }

                core::Order* maker = book_.get_best_ask_order();
                const core::Quantity match_qty =
                    std::min(incoming.remaining_quantity(), maker->remaining_quantity());
                const core::PriceTicks match_price = maker->price();
                const core::OrderId maker_id = maker->order_id();
                const core::Side maker_side = maker->side();

                [[maybe_unused]] const bool tf = incoming.apply_fill(match_qty);
                book_.fill_best_ask_order(match_qty);

                trades.push_back(core::Trade{.trade_id = next_trade_id_++,
                                             .maker_order_id = maker_id,
                                             .taker_order_id = incoming.order_id(),
                                             .maker_side = maker_side,
                                             .price = match_price,
                                             .quantity = match_qty,
                                             .sequence_number = seq});
            }
        } else {
            while (incoming.remaining_quantity() > 0) {
                const auto best_bid = book_.best_bid_price();
                if (!best_bid || incoming.price() > *best_bid) {
                    break;
                }

                core::Order* maker = book_.get_best_bid_order();
                const core::Quantity match_qty =
                    std::min(incoming.remaining_quantity(), maker->remaining_quantity());
                const core::PriceTicks match_price = maker->price();
                const core::OrderId maker_id = maker->order_id();
                const core::Side maker_side = maker->side();

                [[maybe_unused]] const bool tf = incoming.apply_fill(match_qty);
                book_.fill_best_bid_order(match_qty);

                trades.push_back(core::Trade{.trade_id = next_trade_id_++,
                                             .maker_order_id = maker_id,
                                             .taker_order_id = incoming.order_id(),
                                             .maker_side = maker_side,
                                             .price = match_price,
                                             .quantity = match_qty,
                                             .sequence_number = seq});
            }
        }

        core::OrderStatus final_status = core::OrderStatus::New;
        if (incoming.remaining_quantity() == 0) {
            final_status = core::OrderStatus::Filled;
        } else {
            if (trades.empty()) {
                final_status = core::OrderStatus::Resting;
            } else {
                final_status = core::OrderStatus::PartiallyFilled;
            }
            [[maybe_unused]] const bool added = book_.add_order(incoming);
        }

        return core::ExecutionReport{.order_id = incoming.order_id(),
                                     .status = final_status,
                                     .reject_reason = core::RejectReason::None,
                                     .remaining_quantity = incoming.remaining_quantity(),
                                     .filled_quantity = incoming.filled_quantity(),
                                     .price = incoming.price(),
                                     .side = incoming.side(),
                                     .sequence_number = seq,
                                     .trades = std::move(trades)};
    }

    core::ExecutionReport handle_cancel(core::SeqNum seq, const core::CancelOrderCommand& cmd) {
        if (!book_.has_order(cmd.order_id)) {
            return core::ExecutionReport{.order_id = cmd.order_id,
                                         .status = core::OrderStatus::Rejected,
                                         .reject_reason = core::RejectReason::UnknownOrder,
                                         .remaining_quantity = 0,
                                         .filled_quantity = 0,
                                         .price = 0,
                                         .side = core::Side::Buy,
                                         .sequence_number = seq,
                                         .trades = {}};
        }

        auto cancelled_opt = book_.cancel_order(cmd.order_id);
        return core::ExecutionReport{.order_id = cancelled_opt->order_id(),
                                     .status = core::OrderStatus::Cancelled,
                                     .reject_reason = core::RejectReason::None,
                                     .remaining_quantity = 0,
                                     .filled_quantity = cancelled_opt->filled_quantity(),
                                     .price = cancelled_opt->price(),
                                     .side = cancelled_opt->side(),
                                     .sequence_number = seq,
                                     .trades = {}};
    }

    core::ExecutionReport handle_modify(core::SeqNum seq, const core::ModifyOrderCommand& cmd) {
        if (cmd.new_price <= 0) {
            return core::ExecutionReport{.order_id = cmd.order_id,
                                         .status = core::OrderStatus::Rejected,
                                         .reject_reason = core::RejectReason::InvalidPrice,
                                         .remaining_quantity = 0,
                                         .filled_quantity = 0,
                                         .price = cmd.new_price,
                                         .side = core::Side::Buy,
                                         .sequence_number = seq,
                                         .trades = {}};
        }

        if (cmd.new_quantity == 0) {
            return core::ExecutionReport{.order_id = cmd.order_id,
                                         .status = core::OrderStatus::Rejected,
                                         .reject_reason = core::RejectReason::InvalidQuantity,
                                         .remaining_quantity = 0,
                                         .filled_quantity = 0,
                                         .price = cmd.new_price,
                                         .side = core::Side::Buy,
                                         .sequence_number = seq,
                                         .trades = {}};
        }

        const core::Order* existing = book_.get_order(cmd.order_id);
        if (existing == nullptr) {
            return core::ExecutionReport{.order_id = cmd.order_id,
                                         .status = core::OrderStatus::Rejected,
                                         .reject_reason = core::RejectReason::UnknownOrder,
                                         .remaining_quantity = 0,
                                         .filled_quantity = 0,
                                         .price = cmd.new_price,
                                         .side = core::Side::Buy,
                                         .sequence_number = seq,
                                         .trades = {}};
        }

        const core::Side side = existing->side();
        const core::PriceTicks old_price = existing->price();
        const core::Quantity old_remaining = existing->remaining_quantity();
        const core::Quantity old_filled = existing->filled_quantity();

        // Guard: reject if the new price would immediately cross the opposing best.
        // A modify that crosses the spread is semantically an aggressive order, not a
        // modification. The caller must explicitly cancel-then-place if that is the intent.
        // The original order remains in the book untouched on rejection.
        if (side == core::Side::Buy) {
            const auto best_ask = book_.best_ask_price();
            if (best_ask.has_value() && cmd.new_price >= *best_ask) {
                return core::ExecutionReport{
                    .order_id = cmd.order_id,
                    .status = core::OrderStatus::Rejected,
                    .reject_reason = core::RejectReason::ModifyCrossesSpread,
                    .remaining_quantity = old_remaining,
                    .filled_quantity = old_filled,
                    .price = old_price,
                    .side = side,
                    .sequence_number = seq,
                    .trades = {}};
            }
        } else {
            const auto best_bid = book_.best_bid_price();
            if (best_bid.has_value() && cmd.new_price <= *best_bid) {
                return core::ExecutionReport{
                    .order_id = cmd.order_id,
                    .status = core::OrderStatus::Rejected,
                    .reject_reason = core::RejectReason::ModifyCrossesSpread,
                    .remaining_quantity = old_remaining,
                    .filled_quantity = old_filled,
                    .price = old_price,
                    .side = side,
                    .sequence_number = seq,
                    .trades = {}};
            }
        }

        // Safe to cancel: new price is validated non-crossing.
        [[maybe_unused]] auto cancelled_opt = book_.cancel_order(cmd.order_id);

        return handle_create(
            seq, core::CreateOrderCommand{cmd.order_id, side, cmd.new_price, cmd.new_quantity});
    }

    BookType book_;
    core::SeqNum next_sequence_{1};
    core::TradeId next_trade_id_{1};
};

using ReferenceMatchingEngine = GenericMatchingEngine<reference::ReferenceOrderBook>;
using OptimizedMatchingEngine = GenericMatchingEngine<optimized::OptimizedOrderBook>;
using MatchingEngine = ReferenceMatchingEngine;

}  // namespace quantengine::engine
