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

// quantengine/replay/ReplayGateway.hpp
//
// Drop-in IExecutionGateway implementation for historical L3 execution simulation.
//
// RESPONSIBILITIES:
//   1. Satisfies IExecutionGateway interface: seamlessly plugs into StrategyRunner
//      and Portfolio.
//   2. Handles passive limit orders by tracking queue position in QueuePositionTracker.
//   3. Handles aggressive orders (market or crossing limit) by immediate execution
//      against resting historical book liquidity in HistoricalL3Book.
//   4. Supports deterministic virtual latency:
//      - Entry latency: simulated orders arrive at the venue after entry delay.
//      - Response latency: execution reports return to client after response delay.
//   5. Direct wiring to Portfolio/P&L ledger via IFillHandler.

#include <cstdint>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "quantengine/core/events.hpp"
#include "quantengine/execution/IExecutionGateway.hpp"
#include "quantengine/portfolio/Portfolio.hpp"
#include "quantengine/replay/EventClock.hpp"
#include "quantengine/replay/HistoricalL3Book.hpp"
#include "quantengine/replay/LatencyModel.hpp"
#include "quantengine/replay/QueuePositionTracker.hpp"
#include "quantengine/replay/VirtualTimeline.hpp"

namespace quantengine::replay {

class ReplayGateway final : public execution::IExecutionGateway {
public:
    ReplayGateway(HistoricalL3Book& book, QueuePositionTracker& tracker, EventClock& clock,
                  LatencyConfig latency = LatencyConfig::zero(),
                  VirtualTimeline* timeline = nullptr) noexcept;

    ~ReplayGateway() override = default;

    ReplayGateway(const ReplayGateway&) = delete;
    ReplayGateway& operator=(const ReplayGateway&) = delete;
    ReplayGateway(ReplayGateway&&) = delete;
    ReplayGateway& operator=(ReplayGateway&&) = delete;

    // ---- IExecutionGateway --------------------------------------------------

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
        return "ReplayExecutionSimulator";
    }

    // ---- Historical Feed Integration ----------------------------------------

    // Feed step: processes an incoming historical L3 message, updates the book,
    // depletes queue position, and delivers any generated fills.
    void process_historical_message(const L3Message& msg) noexcept;

    // Direct symbol mapping query
    [[nodiscard]] auto find_symbol(core::OrderId id) const noexcept
        -> std::optional<market::SymbolArray>;

    // Latency configuration accessor
    [[nodiscard]] auto latency() const noexcept -> const LatencyConfig& { return latency_; }

private:
    void execute_aggressive(const execution::OrderRequest& request) noexcept;
    void place_passive(const execution::OrderRequest& request) noexcept;
    void deliver_report(const core::ExecutionReport& report,
                        const market::SymbolArray& symbol) noexcept;

    HistoricalL3Book& book_;
    QueuePositionTracker& tracker_;
    EventClock& clock_;
    LatencyConfig latency_;
    VirtualTimeline* timeline_{nullptr};

    execution::IFillHandler* handler_{nullptr};
    bool connected_{false};

    std::unordered_map<core::OrderId, execution::OrderRequest> active_requests_;
    core::SeqNum next_seq_{1};
    core::TradeId next_trade_id_{1};
};

}  // namespace quantengine::replay
