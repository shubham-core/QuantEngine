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

// src/broker/AlpacaGateway.cpp
//
// M10 & M11 AlpacaGateway implementation.

#include "quantengine/broker/AlpacaGateway.hpp"

#include <cstdio>
#include <sstream>

namespace quantengine::broker {

AlpacaGateway::AlpacaGateway(const AlpacaGatewayConfig& config) noexcept : config_(config) {}

AlpacaGateway::~AlpacaGateway() {
    disconnect();
}

bool AlpacaGateway::connect(execution::IFillHandler& handler) noexcept {
    const std::lock_guard<std::mutex> lock(mutex_);
    if (connected_.load(std::memory_order_acquire)) {
        return false;
    }

    fill_handler_ = &handler;
    connected_.store(true, std::memory_order_release);
    fill_handler_->on_gateway_connected();
    return true;
}

void AlpacaGateway::disconnect() noexcept {
    const std::lock_guard<std::mutex> lock(mutex_);
    if (!connected_.load(std::memory_order_acquire)) {
        return;
    }

    connected_.store(false, std::memory_order_release);
    if (fill_handler_ != nullptr) {
        fill_handler_->on_gateway_disconnected("GRACEFUL");
        fill_handler_ = nullptr;
    }
}

bool AlpacaGateway::is_connected() const noexcept {
    return connected_.load(std::memory_order_acquire);
}

std::string AlpacaGateway::format_order_json(const execution::OrderRequest& req) noexcept {
    std::ostringstream oss;
    const auto sym = market::symbol_view(req.symbol);
    const double price_usd = static_cast<double>(req.price) / 100.0;

    oss << "{" << "\"symbol\":\"" << sym << "\"," << "\"qty\":\"" << req.quantity << "\","
        << "\"side\":\"" << (req.side == core::Side::Buy ? "buy" : "sell") << "\"," << "\"type\":\""
        << (req.type == core::OrderType::Limit ? "limit" : "market") << "\","
        << "\"time_in_force\":\"" << execution::to_string(req.time_in_force) << "\","
        << "\"limit_price\":\"" << price_usd << "\"," << "\"client_order_id\":\""
        << req.client_order_id << "\"" << "}";
    return oss.str();
}

execution::OrderAck AlpacaGateway::submit_order(const execution::OrderRequest& request) noexcept {
    if (!is_connected()) {
        return execution::OrderAck{.client_order_id = request.client_order_id,
                                   .status = execution::GatewayStatus::NotConnected,
                                   .reject_reason = core::RejectReason::None};
    }

    if (request.price <= 0 || request.quantity == 0) {
        return execution::OrderAck{.client_order_id = request.client_order_id,
                                   .status = execution::GatewayStatus::Rejected,
                                   .reject_reason = (request.price <= 0)
                                                        ? core::RejectReason::InvalidPrice
                                                        : core::RejectReason::InvalidQuantity};
    }

    // M11: Register order in state machine
    if (!state_machine_.register_order(request, execution::InFlightState::PendingAck)) {
        return execution::OrderAck{.client_order_id = request.client_order_id,
                                   .status = execution::GatewayStatus::Rejected,
                                   .reject_reason = core::RejectReason::DuplicateOrderId};
    }

    // Generate simulated venue order id (e.g. "alpaca-ord-1")
    std::string venue_id;
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        venue_id = "alpaca-ord-" + std::to_string(venue_counter_++);
    }
    state_machine_.associate_venue_id(request.client_order_id, venue_id);

    if (config_.simulate_venue_acks) {
        // Transition to Resting
        state_machine_.transition(request.client_order_id, execution::InFlightState::Resting);

        // Notify fill handler of Resting status
        core::ExecutionReport rpt{};
        rpt.order_id = request.client_order_id;
        rpt.status = core::OrderStatus::Resting;
        rpt.reject_reason = core::RejectReason::None;
        rpt.remaining_quantity = request.quantity;
        rpt.filled_quantity = 0;
        rpt.price = request.price;
        rpt.side = request.side;

        const std::lock_guard<std::mutex> lock(mutex_);
        if (fill_handler_ != nullptr) {
            fill_handler_->on_fill(rpt);
        }
    }

    return execution::OrderAck{.client_order_id = request.client_order_id,
                               .status = execution::GatewayStatus::Accepted,
                               .reject_reason = core::RejectReason::None};
}

execution::OrderAck AlpacaGateway::cancel_order(const execution::CancelRequest& request) noexcept {
    if (!is_connected()) {
        return execution::OrderAck{.client_order_id = request.client_order_id,
                                   .status = execution::GatewayStatus::NotConnected};
    }

    const auto order_opt = state_machine_.get_order(request.client_order_id);
    if (!order_opt) {
        return execution::OrderAck{.client_order_id = request.client_order_id,
                                   .status = execution::GatewayStatus::Rejected,
                                   .reject_reason = core::RejectReason::UnknownOrder};
    }

    // Transition state
    state_machine_.transition(request.client_order_id, execution::InFlightState::PendingCancel);

    if (config_.simulate_venue_acks) {
        state_machine_.transition(request.client_order_id, execution::InFlightState::Cancelled);

        core::ExecutionReport rpt{};
        rpt.order_id = request.client_order_id;
        rpt.status = core::OrderStatus::Cancelled;
        rpt.remaining_quantity = 0;
        rpt.filled_quantity = order_opt->filled_quantity;
        rpt.price = order_opt->request.price;
        rpt.side = order_opt->request.side;

        const std::lock_guard<std::mutex> lock(mutex_);
        if (fill_handler_ != nullptr) {
            fill_handler_->on_fill(rpt);
        }
    }

    return execution::OrderAck{.client_order_id = request.client_order_id,
                               .status = execution::GatewayStatus::Accepted};
}

execution::OrderAck AlpacaGateway::modify_order(const execution::ModifyRequest& request) noexcept {
    if (!is_connected()) {
        return execution::OrderAck{.client_order_id = request.client_order_id,
                                   .status = execution::GatewayStatus::NotConnected};
    }

    const auto order_opt = state_machine_.get_order(request.client_order_id);
    if (!order_opt) {
        return execution::OrderAck{.client_order_id = request.client_order_id,
                                   .status = execution::GatewayStatus::Rejected,
                                   .reject_reason = core::RejectReason::UnknownOrder};
    }

    state_machine_.transition(request.client_order_id, execution::InFlightState::PendingModify);
    state_machine_.transition(request.client_order_id, execution::InFlightState::Resting);

    return execution::OrderAck{.client_order_id = request.client_order_id,
                               .status = execution::GatewayStatus::Accepted};
}

void AlpacaGateway::inject_venue_fill(core::OrderId client_order_id, core::PriceTicks fill_price,
                                      core::Quantity fill_qty) noexcept {
    const auto order_opt = state_machine_.get_order(client_order_id);
    if (!order_opt) {
        return;
    }

    state_machine_.apply_fill(client_order_id, fill_qty);
    const auto updated = state_machine_.get_order(client_order_id);

    core::ExecutionReport rpt{};
    rpt.order_id = client_order_id;
    rpt.status = (updated && updated->remaining_quantity == 0) ? core::OrderStatus::Filled
                                                               : core::OrderStatus::PartiallyFilled;
    rpt.price = fill_price;
    rpt.filled_quantity = updated ? updated->filled_quantity : fill_qty;
    rpt.remaining_quantity = updated ? updated->remaining_quantity : 0;
    rpt.side = order_opt->request.side;

    core::Trade trade{};
    trade.trade_id = 1;
    trade.taker_order_id = client_order_id;
    trade.maker_order_id = 0;
    trade.maker_side =
        (order_opt->request.side == core::Side::Buy) ? core::Side::Sell : core::Side::Buy;
    trade.price = fill_price;
    trade.quantity = fill_qty;
    rpt.trades.push_back(trade);

    const std::lock_guard<std::mutex> lock(mutex_);
    if (fill_handler_ != nullptr) {
        fill_handler_->on_fill(rpt);
    }
}

std::size_t AlpacaGateway::reconcile_open_orders() noexcept {
    return state_machine_.purge_terminal_orders();
}

}  // namespace quantengine::broker
