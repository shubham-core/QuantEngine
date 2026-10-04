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
#include <cstddef>
#include <cstdint>
#include <random>
#include <vector>

#include "quantengine/core/events.hpp"
#include "quantengine/core/types.hpp"

namespace quantengine::bench {

enum class WorkloadType {
    Balanced,       // 50% Adds, 25% Cancels, 25% Crossings
    AddHeavy,       // 90% Non-crossing Adds, 10% Cancels
    CrossingHeavy,  // 30% Adds, 70% Aggressive Sweeps across multiple levels
    CancelHeavy     // 50% Rapid Adds, 50% Immediate Cancels
};

[[nodiscard]] inline const char* workload_name(WorkloadType type) noexcept {
    switch (type) {
        case WorkloadType::Balanced:
            return "Balanced (50% Add, 25% Cancel, 25% Trade)";
        case WorkloadType::AddHeavy:
            return "Add-Heavy (90% Add, 10% Cancel)";
        case WorkloadType::CrossingHeavy:
            return "Crossing/Sweep-Heavy (30% Add, 70% Sweeps)";
        case WorkloadType::CancelHeavy:
            return "Cancel-Heavy (50% Add, 50% Cancel)";
    }
    return "Unknown";
}

class WorkloadGenerator {
public:
    [[nodiscard]] static std::vector<core::OrderCommand> generate(WorkloadType type,
                                                                  std::size_t count,
                                                                  std::uint64_t seed = 42) {
        std::mt19937_64 rng(seed);
        std::vector<core::OrderCommand> commands;
        commands.reserve(count);

        std::vector<core::OrderId> resting_bids;
        std::vector<core::OrderId> resting_asks;
        resting_bids.reserve(count / 2);
        resting_asks.reserve(count / 2);

        core::OrderId next_order_id = 1;
        core::SeqNum seq = 1;

        std::uniform_int_distribution<int> pct_dist(0, 99);
        std::uniform_int_distribution<int64_t> bid_price_dist(9500, 9999);
        std::uniform_int_distribution<int64_t> ask_price_dist(10001, 10500);
        std::uniform_int_distribution<uint64_t> normal_qty_dist(10, 100);
        std::uniform_int_distribution<uint64_t> sweep_qty_dist(100, 500);

        for (std::size_t i = 0; i < count; ++i) {
            const int roll = pct_dist(rng);

            switch (type) {
                case WorkloadType::Balanced: {
                    if (roll < 50 || (resting_bids.empty() && resting_asks.empty())) {
                        const bool is_buy = (pct_dist(rng) < 50);
                        const auto id = next_order_id++;
                        const auto side = is_buy ? core::Side::Buy : core::Side::Sell;
                        const auto price = is_buy ? bid_price_dist(rng) : ask_price_dist(rng);
                        const auto qty = normal_qty_dist(rng);

                        if (is_buy) {
                            resting_bids.push_back(id);
                        } else {
                            resting_asks.push_back(id);
                        }

                        commands.push_back(core::OrderCommand{
                            .sequence_number = seq++,
                            .payload = core::CreateOrderCommand{id, side, price, qty}});
                    } else if (roll < 75) {
                        std::vector<core::OrderId>& pool =
                            (!resting_bids.empty() && (pct_dist(rng) < 50 || resting_asks.empty()))
                                ? resting_bids
                                : resting_asks;

                        std::uniform_int_distribution<std::size_t> idx_dist(0, pool.size() - 1);
                        const auto idx = idx_dist(rng);
                        const auto id = pool[idx];
                        pool[idx] = pool.back();
                        pool.pop_back();

                        commands.push_back(core::OrderCommand{
                            .sequence_number = seq++, .payload = core::CancelOrderCommand{id}});
                    } else {
                        const bool is_taker_buy = (pct_dist(rng) < 50);
                        const auto id = next_order_id++;
                        const auto side = is_taker_buy ? core::Side::Buy : core::Side::Sell;
                        const auto price = is_taker_buy ? ask_price_dist(rng) : bid_price_dist(rng);
                        const auto qty = normal_qty_dist(rng);

                        commands.push_back(core::OrderCommand{
                            .sequence_number = seq++,
                            .payload = core::CreateOrderCommand{id, side, price, qty}});
                    }
                    break;
                }

                case WorkloadType::AddHeavy: {
                    if (roll < 90 || (resting_bids.empty() && resting_asks.empty())) {
                        const bool is_buy = (pct_dist(rng) < 50);
                        const auto id = next_order_id++;
                        const auto side = is_buy ? core::Side::Buy : core::Side::Sell;
                        const auto price = is_buy ? bid_price_dist(rng) : ask_price_dist(rng);
                        const auto qty = normal_qty_dist(rng);

                        if (is_buy) {
                            resting_bids.push_back(id);
                        } else {
                            resting_asks.push_back(id);
                        }

                        commands.push_back(core::OrderCommand{
                            .sequence_number = seq++,
                            .payload = core::CreateOrderCommand{id, side, price, qty}});
                    } else {
                        std::vector<core::OrderId>& pool =
                            (!resting_bids.empty() && (pct_dist(rng) < 50 || resting_asks.empty()))
                                ? resting_bids
                                : resting_asks;

                        std::uniform_int_distribution<std::size_t> idx_dist(0, pool.size() - 1);
                        const auto idx = idx_dist(rng);
                        const auto id = pool[idx];
                        pool[idx] = pool.back();
                        pool.pop_back();

                        commands.push_back(core::OrderCommand{
                            .sequence_number = seq++, .payload = core::CancelOrderCommand{id}});
                    }
                    break;
                }

                case WorkloadType::CrossingHeavy: {
                    if (resting_bids.size() < 20 || resting_asks.size() < 20 || roll < 30) {
                        const bool is_buy = (pct_dist(rng) < 50);
                        const auto id = next_order_id++;
                        const auto side = is_buy ? core::Side::Buy : core::Side::Sell;
                        const auto price = is_buy ? bid_price_dist(rng) : ask_price_dist(rng);
                        const auto qty = normal_qty_dist(rng);

                        if (is_buy) {
                            resting_bids.push_back(id);
                        } else {
                            resting_asks.push_back(id);
                        }

                        commands.push_back(core::OrderCommand{
                            .sequence_number = seq++,
                            .payload = core::CreateOrderCommand{id, side, price, qty}});
                    } else {
                        const bool is_taker_buy = (pct_dist(rng) < 50);
                        const auto id = next_order_id++;
                        const auto side = is_taker_buy ? core::Side::Buy : core::Side::Sell;
                        const auto price = is_taker_buy ? 10600 : 9400;
                        const auto qty = sweep_qty_dist(rng);

                        commands.push_back(core::OrderCommand{
                            .sequence_number = seq++,
                            .payload = core::CreateOrderCommand{id, side, price, qty}});
                    }
                    break;
                }

                case WorkloadType::CancelHeavy: {
                    if (roll < 50 || (resting_bids.empty() && resting_asks.empty())) {
                        const bool is_buy = (pct_dist(rng) < 50);
                        const auto id = next_order_id++;
                        const auto side = is_buy ? core::Side::Buy : core::Side::Sell;
                        const auto price = is_buy ? bid_price_dist(rng) : ask_price_dist(rng);
                        const auto qty = normal_qty_dist(rng);

                        if (is_buy) {
                            resting_bids.push_back(id);
                        } else {
                            resting_asks.push_back(id);
                        }

                        commands.push_back(core::OrderCommand{
                            .sequence_number = seq++,
                            .payload = core::CreateOrderCommand{id, side, price, qty}});
                    } else {
                        std::vector<core::OrderId>& pool =
                            (!resting_bids.empty() && (pct_dist(rng) < 50 || resting_asks.empty()))
                                ? resting_bids
                                : resting_asks;

                        std::uniform_int_distribution<std::size_t> idx_dist(0, pool.size() - 1);
                        const auto idx = idx_dist(rng);
                        const auto id = pool[idx];
                        pool[idx] = pool.back();
                        pool.pop_back();

                        commands.push_back(core::OrderCommand{
                            .sequence_number = seq++, .payload = core::CancelOrderCommand{id}});
                    }
                    break;
                }
            }
        }

        return commands;
    }
};

}  // namespace quantengine::bench
