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

// src/execution/SimGateway.cpp
//
// SimGateway implementation.
// See header for full contract and design documentation.

#include "quantengine/execution/SimGateway.hpp"

namespace quantengine::execution {

// ---------------------------------------------------------------------------
auto SimGateway::connect(IFillHandler& handler) noexcept -> bool {
    if (connected_) {
        return false;
    }
    fill_handler_ = &handler;
    connected_ = true;
    handler.on_gateway_connected();
    return true;
}

void SimGateway::disconnect() noexcept {
    connected_ = false;
    if (fill_handler_ != nullptr) {
        fill_handler_->on_gateway_disconnected("GRACEFUL");
    }
}

// ---------------------------------------------------------------------------
// dispatch_report: translate a core::ExecutionReport and fire on_fill().
// Called for every report the engine returns (resting, partial, filled,
// cancelled, rejected).
// ---------------------------------------------------------------------------
void SimGateway::dispatch_report(const core::ExecutionReport& report) noexcept {
    if (fill_handler_ != nullptr) {
        fill_handler_->on_fill(report);
    }
}

// ---------------------------------------------------------------------------
// submit_order
// ---------------------------------------------------------------------------
auto SimGateway::submit_order(const OrderRequest& request) noexcept -> OrderAck {
    if (!connected_) {
        return OrderAck{.client_order_id = request.client_order_id,
                        .status = GatewayStatus::NotConnected};
    }

    // Validate request parameters before touching the engine.
    if (request.price <= 0 && request.type == core::OrderType::Limit) {
        return OrderAck{.client_order_id = request.client_order_id,
                        .status = GatewayStatus::Rejected,
                        .reject_reason = core::RejectReason::InvalidPrice};
    }
    if (request.quantity == 0) {
        return OrderAck{.client_order_id = request.client_order_id,
                        .status = GatewayStatus::Rejected,
                        .reject_reason = core::RejectReason::InvalidQuantity};
    }

    // Submit to the engine. Uses client_order_id directly as the engine OrderId.
    core::ExecutionReport report = engine_.submit_order(request.client_order_id, request.side,
                                                        request.price, request.quantity);

    // Fire fill callbacks for any trades that occurred during matching.
    // The engine returns all trades in a single report. We deliver it as-is;
    // the fill handler sees trades[] populated alongside the final status.
    dispatch_report(report);

    // IOC: if any quantity remains resting (partially matched or not at all),
    // immediately cancel the residual. Fire a Cancelled report.
    if (request.time_in_force == TimeInForce::Ioc && report.remaining_quantity > 0) {
        const core::ExecutionReport cancel_report = engine_.cancel_order(request.client_order_id);
        dispatch_report(cancel_report);
    }

    return OrderAck{.client_order_id = request.client_order_id, .status = GatewayStatus::Accepted};
}

// ---------------------------------------------------------------------------
// cancel_order
// ---------------------------------------------------------------------------
auto SimGateway::cancel_order(const CancelRequest& request) noexcept -> OrderAck {
    if (!connected_) {
        return OrderAck{.client_order_id = request.client_order_id,
                        .status = GatewayStatus::NotConnected};
    }

    const core::ExecutionReport report = engine_.cancel_order(request.client_order_id);
    dispatch_report(report);

    // If the engine rejected the cancel (unknown order etc.), propagate as Rejected.
    if (report.status == core::OrderStatus::Rejected) {
        return OrderAck{.client_order_id = request.client_order_id,
                        .status = GatewayStatus::Rejected,
                        .reject_reason = report.reject_reason};
    }

    return OrderAck{.client_order_id = request.client_order_id, .status = GatewayStatus::Accepted};
}

// ---------------------------------------------------------------------------
// modify_order
// ---------------------------------------------------------------------------
auto SimGateway::modify_order(const ModifyRequest& request) noexcept -> OrderAck {
    if (!connected_) {
        return OrderAck{.client_order_id = request.client_order_id,
                        .status = GatewayStatus::NotConnected};
    }

    const core::ExecutionReport report =
        engine_.modify_order(request.client_order_id, request.new_price, request.new_quantity);

    dispatch_report(report);

    if (report.status == core::OrderStatus::Rejected) {
        return OrderAck{.client_order_id = request.client_order_id,
                        .status = GatewayStatus::Rejected,
                        .reject_reason = report.reject_reason};
    }

    return OrderAck{.client_order_id = request.client_order_id, .status = GatewayStatus::Accepted};
}

}  // namespace quantengine::execution
