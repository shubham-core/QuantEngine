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

#include "quantengine/replay/ReplayGateway.hpp"

namespace quantengine::replay {

ReplayGateway::ReplayGateway(HistoricalL3Book& book, QueuePositionTracker& tracker,
                             EventClock& clock, LatencyConfig latency,
                             VirtualTimeline* timeline) noexcept
    : book_(book), tracker_(tracker), clock_(clock), latency_(latency), timeline_(timeline) {
    if (timeline_) {
        timeline_->set_venue_arrival_handler([this](const execution::OrderRequest& req) {
            const bool is_market = (req.price == 0);
            bool is_aggressive = false;
            if (req.side == core::Side::Buy) {
                is_aggressive = is_market || (book_.best_ask_price().has_value() &&
                                              req.price >= *book_.best_ask_price());
            } else {
                is_aggressive = is_market || (book_.best_bid_price().has_value() &&
                                              req.price <= *book_.best_bid_price());
            }

            if (is_aggressive) {
                execute_aggressive(req);
            } else {
                place_passive(req);
            }
        });

        timeline_->set_fill_delivery_handler([this](const core::ExecutionReport& report) {
            auto sym_opt = find_symbol(report.order_id);
            market::SymbolArray sym = sym_opt.value_or(market::SymbolArray{});
            if (handler_) {
                auto* port = dynamic_cast<portfolio::Portfolio*>(handler_);
                if (port) {
                    port->apply_fill_with_symbol(report, sym);
                } else {
                    handler_->on_fill(report);
                }
            }
        });
    }
}

auto ReplayGateway::connect(execution::IFillHandler& handler) noexcept -> bool {
    if (connected_)
        return false;
    handler_ = &handler;
    connected_ = true;
    handler_->on_gateway_connected();
    return true;
}

void ReplayGateway::disconnect() noexcept {
    if (connected_) {
        connected_ = false;
        if (handler_) {
            handler_->on_gateway_disconnected("client_disconnect");
        }
    }
}

auto ReplayGateway::is_connected() const noexcept -> bool {
    return connected_;
}

auto ReplayGateway::submit_order(const execution::OrderRequest& request) noexcept
    -> execution::OrderAck {
    if (!connected_) {
        return execution::OrderAck{
            .client_order_id = request.client_order_id,
            .status = execution::GatewayStatus::NotConnected,
            .reject_reason = core::RejectReason::None,
        };
    }

    if (request.client_order_id == 0 || request.quantity == 0) {
        return execution::OrderAck{
            .client_order_id = request.client_order_id,
            .status = execution::GatewayStatus::Rejected,
            .reject_reason = core::RejectReason::InvalidQuantity,
        };
    }

    if (request.price < 0) {
        return execution::OrderAck{
            .client_order_id = request.client_order_id,
            .status = execution::GatewayStatus::Rejected,
            .reject_reason = core::RejectReason::InvalidPrice,
        };
    }

    if (active_requests_.contains(request.client_order_id)) {
        return execution::OrderAck{
            .client_order_id = request.client_order_id,
            .status = execution::GatewayStatus::Rejected,
            .reject_reason = core::RejectReason::DuplicateOrderId,
        };
    }

    active_requests_.emplace(request.client_order_id, request);

    const market::NanoTs venue_arrival_ts = clock_.current_time() + latency_.entry_latency_ns;

    if (timeline_ != nullptr && latency_.entry_latency_ns > 0) {
        timeline_->schedule(venue_arrival_ts, TimelineEventPriority::VenueArrival,
                            VenueArrivalPayload{.request = request});
    } else {
        // Synchronous arrival at venue
        const bool is_market = (request.price == 0);
        bool is_aggressive = false;
        if (request.side == core::Side::Buy) {
            is_aggressive = is_market || (book_.best_ask_price().has_value() &&
                                          request.price >= *book_.best_ask_price());
        } else {
            is_aggressive = is_market || (book_.best_bid_price().has_value() &&
                                          request.price <= *book_.best_bid_price());
        }

        if (is_aggressive) {
            execute_aggressive(request);
        } else {
            place_passive(request);
        }
    }

    return execution::OrderAck{
        .client_order_id = request.client_order_id,
        .status = execution::GatewayStatus::Accepted,
        .reject_reason = core::RejectReason::None,
    };
}

auto ReplayGateway::cancel_order(const execution::CancelRequest& request) noexcept
    -> execution::OrderAck {
    if (!connected_) {
        return execution::OrderAck{
            .client_order_id = request.client_order_id,
            .status = execution::GatewayStatus::NotConnected,
        };
    }

    auto it = active_requests_.find(request.client_order_id);
    if (it == active_requests_.end()) {
        return execution::OrderAck{
            .client_order_id = request.client_order_id,
            .status = execution::GatewayStatus::Rejected,
            .reject_reason = core::RejectReason::UnknownOrder,
        };
    }

    tracker_.cancel_order(request.client_order_id);

    core::ExecutionReport report{
        .order_id = request.client_order_id,
        .status = core::OrderStatus::Cancelled,
        .reject_reason = core::RejectReason::None,
        .remaining_quantity = 0,
        .filled_quantity = 0,
        .price = it->second.price,
        .side = it->second.side,
        .sequence_number = ++next_seq_,
        .trades = {},
    };

    deliver_report(report, it->second.symbol);
    active_requests_.erase(it);

    return execution::OrderAck{
        .client_order_id = request.client_order_id,
        .status = execution::GatewayStatus::Accepted,
    };
}

auto ReplayGateway::modify_order(const execution::ModifyRequest& request) noexcept
    -> execution::OrderAck {
    // Modify implemented as cancel + submit
    (void)cancel_order(execution::CancelRequest{.client_order_id = request.client_order_id});

    execution::OrderRequest new_req{
        .client_order_id = request.client_order_id,
        .side = core::Side::Buy,
        .type = core::OrderType::Limit,
        .time_in_force = execution::TimeInForce::Day,
        .symbol = {},
        .price = request.new_price,
        .quantity = request.new_quantity,
    };

    return submit_order(new_req);
}

void ReplayGateway::execute_aggressive(const execution::OrderRequest& request) noexcept {
    const bool is_market = (request.price == 0);
    std::optional<core::PriceTicks> limit_px =
        is_market ? std::nullopt : std::make_optional(request.price);

    auto trades = book_.match_aggressive(request.side, request.quantity, limit_px);

    core::Quantity filled_vol = 0;
    for (auto& t : trades) {
        t.taker_order_id = request.client_order_id;
        filled_vol += t.quantity;
    }

    const core::Quantity rem_vol = request.quantity - filled_vol;

    if (filled_vol > 0) {
        core::ExecutionReport fill_report{
            .order_id = request.client_order_id,
            .status =
                (rem_vol == 0) ? core::OrderStatus::Filled : core::OrderStatus::PartiallyFilled,
            .reject_reason = core::RejectReason::None,
            .remaining_quantity = rem_vol,
            .filled_quantity = filled_vol,
            .price = trades.front().price,
            .side = request.side,
            .sequence_number = ++next_seq_,
            .trades = std::move(trades),
        };
        deliver_report(fill_report, request.symbol);
    }

    if (rem_vol > 0) {
        if (request.time_in_force == execution::TimeInForce::Ioc) {
            core::ExecutionReport cancel_report{
                .order_id = request.client_order_id,
                .status = core::OrderStatus::Cancelled,
                .reject_reason = core::RejectReason::None,
                .remaining_quantity = 0,
                .filled_quantity = filled_vol,
                .price = request.price,
                .side = request.side,
                .sequence_number = ++next_seq_,
                .trades = {},
            };
            deliver_report(cancel_report, request.symbol);
            active_requests_.erase(request.client_order_id);
        } else {
            // Remainder rests passively
            execution::OrderRequest remainder_req = request;
            remainder_req.quantity = rem_vol;
            place_passive(remainder_req);
        }
    } else {
        active_requests_.erase(request.client_order_id);
    }
}

void ReplayGateway::place_passive(const execution::OrderRequest& request) noexcept {
    tracker_.track_order(request.client_order_id, request.side, request.price, request.quantity,
                         book_, clock_.current_time());

    core::ExecutionReport resting_report{
        .order_id = request.client_order_id,
        .status = core::OrderStatus::Resting,
        .reject_reason = core::RejectReason::None,
        .remaining_quantity = request.quantity,
        .filled_quantity = 0,
        .price = request.price,
        .side = request.side,
        .sequence_number = ++next_seq_,
        .trades = {},
    };

    deliver_report(resting_report, request.symbol);
}

void ReplayGateway::deliver_report(const core::ExecutionReport& report,
                                   const market::SymbolArray& symbol) noexcept {
    if (timeline_ != nullptr && latency_.response_latency_ns > 0) {
        const market::NanoTs delivery_ts = clock_.current_time() + latency_.response_latency_ns;
        timeline_->schedule(delivery_ts, TimelineEventPriority::FillDelivery,
                            FillDeliveryPayload{.report = report});
    } else {
        if (handler_) {
            auto* port = dynamic_cast<portfolio::Portfolio*>(handler_);
            if (port) {
                port->apply_fill_with_symbol(report, symbol);
            } else {
                handler_->on_fill(report);
            }
        }
    }
}

void ReplayGateway::process_historical_message(const L3Message& msg) noexcept {
    const market::NanoTs msg_ts = l3_exchange_ts(msg);
    clock_.advance_to(msg_ts);

    std::visit(
        [this](const auto& m) noexcept {
            using T = std::decay_t<decltype(m)>;
            if constexpr (std::is_same_v<T, OrderExecuted>) {
                const auto* existing = book_.get_order(m.venue_order_id);
                HistoricalOrder snapshot{};
                if (existing)
                    snapshot = *existing;

                book_.apply(m);

                auto fills = tracker_.on_historical_execute(m, existing ? &snapshot : nullptr);
                for (const auto& fill : fills) {
                    const auto* sim_ord = tracker_.get_order(fill.client_order_id);
                    auto it = active_requests_.find(fill.client_order_id);
                    market::SymbolArray sym =
                        (it != active_requests_.end()) ? it->second.symbol : market::SymbolArray{};

                    std::vector<core::Trade> trades;
                    trades.push_back(core::Trade{
                        .trade_id = ++next_trade_id_,
                        .maker_order_id = fill.client_order_id,
                        .taker_order_id = 0,
                        .maker_side = fill.side,
                        .price = fill.fill_price,
                        .quantity = fill.fill_quantity,
                        .sequence_number = ++next_seq_,
                    });

                    core::ExecutionReport report{
                        .order_id = fill.client_order_id,
                        .status = (sim_ord && sim_ord->is_filled())
                                      ? core::OrderStatus::Filled
                                      : core::OrderStatus::PartiallyFilled,
                        .reject_reason = core::RejectReason::None,
                        .remaining_quantity = (sim_ord ? sim_ord->remaining_quantity : 0),
                        .filled_quantity = fill.fill_quantity,
                        .price = fill.fill_price,
                        .side = fill.side,
                        .sequence_number = next_seq_,
                        .trades = std::move(trades),
                    };

                    deliver_report(report, sym);

                    if (sim_ord && sim_ord->is_filled()) {
                        active_requests_.erase(fill.client_order_id);
                    }
                }
            } else if constexpr (std::is_same_v<T, OrderCancelled>) {
                const auto* existing = book_.get_order(m.venue_order_id);
                HistoricalOrder snapshot{};
                if (existing)
                    snapshot = *existing;

                book_.apply(m);
                tracker_.on_historical_cancel(m, existing ? &snapshot : nullptr);
            } else {
                book_.apply(m);
            }
        },
        msg);
}

auto ReplayGateway::find_symbol(core::OrderId id) const noexcept
    -> std::optional<market::SymbolArray> {
    auto it = active_requests_.find(id);
    if (it == active_requests_.end())
        return std::nullopt;
    return it->second.symbol;
}

}  // namespace quantengine::replay
