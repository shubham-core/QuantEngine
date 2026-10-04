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

// quantengine/replay/EventClock.hpp
//
// Deterministic virtual event clock for market replay and execution simulation.
//
// GUARANTEES:
//   1. Monotonic: time never moves backwards.
//   2. Strictly virtual: zero calls to std::chrono::system_clock or sleep().
//   3. Deterministic: driven purely by discrete event timestamps and latency offsets.

#include <cstdint>

#include "quantengine/market/MarketEvent.hpp"

namespace quantengine::replay {

class EventClock {
public:
    explicit EventClock(market::NanoTs initial_time = 0) noexcept : current_time_(initial_time) {}

    ~EventClock() = default;
    EventClock(const EventClock&) = default;
    EventClock& operator=(const EventClock&) = default;
    EventClock(EventClock&&) noexcept = default;
    EventClock& operator=(EventClock&&) noexcept = default;

    [[nodiscard]] auto current_time() const noexcept -> market::NanoTs { return current_time_; }

    // Advance virtual time to target_ts.
    // Returns true if time advanced or stayed same; false if target_ts < current_time
    // (non-monotonic).
    auto advance_to(market::NanoTs target_ts) noexcept -> bool {
        if (target_ts < current_time_) {
            return false;
        }
        current_time_ = target_ts;
        return true;
    }

    // Advance virtual time forward by delta_ns.
    auto advance_by(market::NanoTs delta_ns) noexcept -> bool {
        if (delta_ns < 0) {
            return false;
        }
        current_time_ += delta_ns;
        return true;
    }

    void reset(market::NanoTs start_ts = 0) noexcept { current_time_ = start_ts; }

private:
    market::NanoTs current_time_{0};
};

}  // namespace quantengine::replay
