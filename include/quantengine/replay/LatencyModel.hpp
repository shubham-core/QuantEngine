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

// quantengine/replay/LatencyModel.hpp
//
// Strongly-typed virtual latency configuration for historical execution simulation.
//
// In electronic trading, latency is composed of discrete physical segments:
//   1. Feed latency (T_feed -> T_obs): Market data transit from exchange matching engine to
//   strategy.
//   2. Decision latency (T_obs -> T_submit): Strategy computation time.
//   3. Entry latency (T_submit -> T_venue): Order network wire transit from client to exchange
//   gateway.
//   4. Response latency (T_venue -> T_ack): Execution report / fill transit back to strategy.

#include <cstdint>

#include "quantengine/market/MarketEvent.hpp"

namespace quantengine::replay {

struct LatencyConfig {
    market::NanoTs feed_latency_ns{0};      // Exchange to strategy feed transit
    market::NanoTs decision_latency_ns{0};  // Strategy decision computation
    market::NanoTs entry_latency_ns{0};     // Strategy order transit to exchange venue
    market::NanoTs response_latency_ns{0};  // Fill / ack transit back to strategy

    [[nodiscard]] static constexpr auto zero() noexcept -> LatencyConfig { return LatencyConfig{}; }

    [[nodiscard]] static constexpr auto symmetric_network(market::NanoTs one_way_ns) noexcept
        -> LatencyConfig {
        return LatencyConfig{
            .feed_latency_ns = one_way_ns,
            .decision_latency_ns = 0,
            .entry_latency_ns = one_way_ns,
            .response_latency_ns = one_way_ns,
        };
    }

    [[nodiscard]] constexpr auto total_round_trip() const noexcept -> market::NanoTs {
        return entry_latency_ns + response_latency_ns;
    }

    [[nodiscard]] constexpr auto total_pipeline_delay() const noexcept -> market::NanoTs {
        return feed_latency_ns + decision_latency_ns + entry_latency_ns + response_latency_ns;
    }

    [[nodiscard]] constexpr bool operator==(const LatencyConfig&) const noexcept = default;
};

}  // namespace quantengine::replay
