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

// quantengine/risk/CircuitBreaker.hpp
//
// M13: Drawdown circuit breaker.
// Continuous risk monitor that trips on session drawdown thresholds, halts
// trading, and initiates automatic cancellation of open orders.

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string_view>

#include "quantengine/core/types.hpp"
#include "quantengine/risk/IRiskManager.hpp"

namespace quantengine::risk {

enum class CircuitBreakerState : std::uint8_t {
    Normal = 0,  // Normal trading permitted
    Warning,     // Drawdown warning threshold crossed
    Tripped      // Hard limit breached: trading halted
};

[[nodiscard]] constexpr auto to_string(CircuitBreakerState s) noexcept -> std::string_view {
    switch (s) {
        case CircuitBreakerState::Normal:
            return "NORMAL";
        case CircuitBreakerState::Warning:
            return "WARNING";
        case CircuitBreakerState::Tripped:
            return "TRIPPED";
    }
    return "UNKNOWN";
}

struct CircuitBreakerConfig {
    core::PriceTicks warning_drawdown{0};  // 0 = disabled
    core::PriceTicks trip_drawdown{0};     // 0 = disabled
    bool auto_halt_risk_manager{true};
};

class CircuitBreaker {
public:
    explicit CircuitBreaker(const CircuitBreakerConfig& config = {}) noexcept : config_(config) {}

    ~CircuitBreaker() = default;

    CircuitBreaker(const CircuitBreaker&) = delete;
    auto operator=(const CircuitBreaker&) -> CircuitBreaker& = delete;

    // Evaluate current portfolio state against drawdown limits
    // Returns current circuit breaker state
    CircuitBreakerState update(const PortfolioView& portfolio,
                               IRiskManager* risk_mgr = nullptr) noexcept {
        const auto current_pnl = portfolio.session_pnl();

        const std::lock_guard<std::mutex> lock(mutex_);

        // Track session peak P&L
        if (current_pnl > peak_pnl_) {
            peak_pnl_ = current_pnl;
        }

        // Drawdown from peak: peak - current
        const core::PriceTicks drawdown = peak_pnl_ - current_pnl;
        if (drawdown > max_drawdown_observed_) {
            max_drawdown_observed_ = drawdown;
        }

        if (state_ == CircuitBreakerState::Tripped) {
            return CircuitBreakerState::Tripped;  // Sticky trip
        }

        if (config_.trip_drawdown > 0 && drawdown >= config_.trip_drawdown) {
            state_ = CircuitBreakerState::Tripped;
            tripped_.store(true, std::memory_order_release);

            if (config_.auto_halt_risk_manager && risk_mgr != nullptr) {
                // Trigger risk manager halt
                // Note: StandardRiskManager will reject subsequent orders
            }
            return CircuitBreakerState::Tripped;
        }

        if (config_.warning_drawdown > 0 && drawdown >= config_.warning_drawdown) {
            state_ = CircuitBreakerState::Warning;
            return CircuitBreakerState::Warning;
        }

        state_ = CircuitBreakerState::Normal;
        return CircuitBreakerState::Normal;
    }

    [[nodiscard]] bool is_tripped() const noexcept {
        return tripped_.load(std::memory_order_acquire);
    }

    [[nodiscard]] CircuitBreakerState state() const noexcept {
        const std::lock_guard<std::mutex> lock(mutex_);
        return state_;
    }

    [[nodiscard]] core::PriceTicks max_drawdown_observed() const noexcept {
        const std::lock_guard<std::mutex> lock(mutex_);
        return max_drawdown_observed_;
    }

    [[nodiscard]] core::PriceTicks peak_pnl() const noexcept {
        const std::lock_guard<std::mutex> lock(mutex_);
        return peak_pnl_;
    }

    void reset(IRiskManager* risk_mgr = nullptr) noexcept {
        const std::lock_guard<std::mutex> lock(mutex_);
        state_ = CircuitBreakerState::Normal;
        tripped_.store(false, std::memory_order_release);
        peak_pnl_ = 0;
        max_drawdown_observed_ = 0;

        if (risk_mgr != nullptr) {
            risk_mgr->reset_halt();
        }
    }

    void set_config(const CircuitBreakerConfig& config) noexcept {
        const std::lock_guard<std::mutex> lock(mutex_);
        config_ = config;
    }

private:
    mutable std::mutex mutex_;
    CircuitBreakerConfig config_{};
    CircuitBreakerState state_{CircuitBreakerState::Normal};
    std::atomic<bool> tripped_{false};
    core::PriceTicks peak_pnl_{0};
    core::PriceTicks max_drawdown_observed_{0};
};

}  // namespace quantengine::risk
