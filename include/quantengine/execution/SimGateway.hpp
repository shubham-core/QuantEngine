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

// quantengine/execution/SimGateway.hpp
//
// IExecutionGateway backed by the OptimizedMatchingEngine.
// Designed for paper-trading simulation: execution is synchronous and
// deterministic — on_fill() is called within submit_order() before it returns.
//
// SEMANTICS:
//   - submit_order():  converts OrderRequest → CreateOrderCommand, runs engine,
//     translates every trade and status update into ExecutionReport callbacks.
//     For IOC orders: if any quantity remains resting after match, it is
//     immediately cancelled and a Cancelled report is fired.
//   - cancel_order():  translates CancelRequest → CancelOrderCommand.
//   - modify_order():  translates ModifyRequest → ModifyOrderCommand.
//     ModifyCrossesSpread is propagated as GatewayStatus::Rejected.
//   - client_order_id is used directly as the engine's OrderId.
//     IDs must be unique per gateway lifetime (no session reset support yet).
//
// THREAD SAFETY:
//   Not thread-safe. All calls must come from a single thread.
//   For multi-threaded use, wrap with an external lock or a command queue.
//
// ALLOCATION:
//   Hot path (submit_order → engine → on_fill) allocates only for
//   ExecutionReport.trades (std::vector<Trade>) when trades occur.
//   The engine itself is zero-allocation for order nodes.
//   A future optimisation is a small_vector<Trade, 4> to eliminate the
//   vector allocation for the common single-fill case.

#include <string_view>

#include "quantengine/core/types.hpp"
#include "quantengine/engine/generic_matching_engine.hpp"
#include "quantengine/execution/IExecutionGateway.hpp"
#include "quantengine/optimized/optimized_order_book.hpp"

namespace quantengine::execution {

class SimGateway final : public IExecutionGateway {
public:
    SimGateway() = default;
    ~SimGateway() override = default;

    SimGateway(const SimGateway&) = delete;
    auto operator=(const SimGateway&) -> SimGateway& = delete;
    SimGateway(SimGateway&&) = delete;
    auto operator=(SimGateway&&) -> SimGateway& = delete;

    // Register the fill handler and mark gateway as connected.
    // Returns false if already connected.
    [[nodiscard]] auto connect(IFillHandler& handler) noexcept -> bool override;

    // Disconnect. Fires on_gateway_disconnected("GRACEFUL").
    void disconnect() noexcept override;

    [[nodiscard]] auto is_connected() const noexcept -> bool override { return connected_; }

    // Submit a new limit (or IOC) order.
    // For DAY/GTC: resting reports arrive via on_fill(); trades arrive as they match.
    // For IOC: unmatched remainder is cancelled synchronously; Cancelled report fired.
    [[nodiscard]] auto submit_order(const OrderRequest& request) noexcept -> OrderAck override;

    [[nodiscard]] auto cancel_order(const CancelRequest& request) noexcept -> OrderAck override;

    // Modify is a cancel-then-reinsert in the engine.
    // Returns Rejected (ModifyCrossesSpread) if new price would cross opposing best.
    [[nodiscard]] auto modify_order(const ModifyRequest& request) noexcept -> OrderAck override;

    [[nodiscard]] auto gateway_name() const noexcept -> std::string_view override {
        return "SimGateway";
    }

    // Access the underlying engine for inspection/testing.
    // Do NOT mutate the engine directly while orders are in-flight.
    [[nodiscard]] auto engine() const noexcept -> const engine::OptimizedMatchingEngine& {
        return engine_;
    }

    // Reset the engine and all state (for test isolation).
    void reset() noexcept { engine_.reset(); }

private:
    // Translate a core::ExecutionReport → call fill_handler_->on_fill().
    // Fires one report per fill + one for the final resting/cancelled/rejected state.
    void dispatch_report(const core::ExecutionReport& report) noexcept;

    engine::OptimizedMatchingEngine engine_;
    IFillHandler* fill_handler_{nullptr};
    bool connected_{false};
};

}  // namespace quantengine::execution
