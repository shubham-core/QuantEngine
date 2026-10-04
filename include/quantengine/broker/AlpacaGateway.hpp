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

// quantengine/broker/AlpacaGateway.hpp
//
// M10 & M11: Alpaca execution gateway implementing IExecutionGateway.
// Integrates OrderStateMachine for in-flight tracking, generates idempotency
// keys, formats JSON REST order payloads, and manages reconciliation.

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>

#include "quantengine/execution/IExecutionGateway.hpp"
#include "quantengine/execution/OrderStateMachine.hpp"

namespace quantengine::broker {

struct AlpacaGatewayConfig {
    std::string api_key{};
    std::string secret_key{};
    std::string base_url{"https://paper-api.alpaca.markets"};
    bool is_paper_trading{true};
    bool simulate_venue_acks{true};  // Synchronous simulated venue response
};

class AlpacaGateway final : public execution::IExecutionGateway {
public:
    explicit AlpacaGateway(const AlpacaGatewayConfig& config = {}) noexcept;
    ~AlpacaGateway() override;

    AlpacaGateway(const AlpacaGateway&) = delete;
    auto operator=(const AlpacaGateway&) -> AlpacaGateway& = delete;
    AlpacaGateway(AlpacaGateway&&) = delete;
    auto operator=(AlpacaGateway&&) -> AlpacaGateway& = delete;

    // ---- IExecutionGateway implementation -----------------------------------

    [[nodiscard]] auto connect(execution::IFillHandler& handler) noexcept -> bool override;
    void disconnect() noexcept override;
    [[nodiscard]] auto is_connected() const noexcept -> bool override;

    [[nodiscard]] auto submit_order(const execution::OrderRequest& request) noexcept
        -> execution::OrderAck override;

    [[nodiscard]] auto cancel_order(const execution::CancelRequest& request) noexcept
        -> execution::OrderAck override;

    [[nodiscard]] auto modify_order(const execution::ModifyRequest& request) noexcept
        -> execution::OrderAck override;

    [[nodiscard]] auto gateway_name() const noexcept -> std::string_view override {
        return "AlpacaGateway";
    }

    // ---- M10 & M11 Venue Simulation & State Machine Access ------------------

    [[nodiscard]] execution::OrderStateMachine& state_machine() noexcept { return state_machine_; }
    [[nodiscard]] const execution::OrderStateMachine& state_machine() const noexcept {
        return state_machine_;
    }

    // Helper: format Alpaca JSON order request payload
    [[nodiscard]] static std::string format_order_json(const execution::OrderRequest& req) noexcept;

    // Inject fill from venue (e.g. from Alpaca trade update message)
    void inject_venue_fill(core::OrderId client_order_id, core::PriceTicks fill_price,
                           core::Quantity fill_qty) noexcept;

    // Reconcile in-flight orders after reconnect (M11/M12)
    std::size_t reconcile_open_orders() noexcept;

private:
    AlpacaGatewayConfig config_{};
    mutable std::mutex mutex_;
    execution::IFillHandler* fill_handler_{nullptr};
    execution::OrderStateMachine state_machine_{};
    std::atomic<bool> connected_{false};
    std::uint64_t venue_counter_{1};
};

}  // namespace quantengine::broker
