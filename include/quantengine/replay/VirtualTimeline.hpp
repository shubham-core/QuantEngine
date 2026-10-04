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

// quantengine/replay/VirtualTimeline.hpp
//
// Deterministic discrete-event simulation timeline.
//
// Schedules and coordinates events across virtual time:
//   - Historical feed events
//   - Strategy observations (delayed by feed latency)
//   - Order submissions & venue arrivals (delayed by entry latency)
//   - Execution reports & fills (delayed by response latency)
//
// Strict determinism: events at identical timestamps are ordered by explicit
// priority and a monotonic schedule ID. Zero wall-clock dependencies.

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <queue>
#include <variant>
#include <vector>

#include "quantengine/core/events.hpp"
#include "quantengine/execution/IExecutionGateway.hpp"
#include "quantengine/market/MarketEvent.hpp"
#include "quantengine/replay/EventClock.hpp"
#include "quantengine/replay/L3Message.hpp"
#include "quantengine/replay/QueuePositionTracker.hpp"

namespace quantengine::replay {

enum class TimelineEventPriority : std::uint8_t {
    VenueArrival = 0,         // Order reaches exchange venue
    FeedMessage = 1,          // Historical exchange market event
    StrategyObservation = 2,  // Strategy observes feed
    OrderSubmission = 3,      // Strategy submits order
    FillDelivery = 4,         // Fill / Ack report reaches strategy
    Action = 5,               // Custom callback action
};

// Event payload types
struct FeedEventPayload {
    L3Message message;
};

struct StrategyObservationPayload {
    market::MarketEvent event;
};

struct OrderSubmissionPayload {
    execution::OrderRequest request;
};

struct VenueArrivalPayload {
    execution::OrderRequest request;
};

struct FillDeliveryPayload {
    core::ExecutionReport report;
};

using TimelineAction = std::function<void(EventClock&)>;

using TimelineEventPayload =
    std::variant<FeedEventPayload, StrategyObservationPayload, OrderSubmissionPayload,
                 VenueArrivalPayload, FillDeliveryPayload, TimelineAction>;

struct TimelineEvent {
    market::NanoTs timestamp{0};
    TimelineEventPriority priority{TimelineEventPriority::Action};
    std::uint64_t schedule_id{0};  // Monotonic tie-breaker
    TimelineEventPayload payload;

    [[nodiscard]] constexpr bool operator>(const TimelineEvent& other) const noexcept {
        if (timestamp != other.timestamp) {
            return timestamp > other.timestamp;
        }
        if (priority != other.priority) {
            return static_cast<std::uint8_t>(priority) > static_cast<std::uint8_t>(other.priority);
        }
        return schedule_id > other.schedule_id;
    }
};

class VirtualTimeline {
public:
    explicit VirtualTimeline(EventClock& clock) noexcept : clock_(clock) {}

    ~VirtualTimeline() = default;
    VirtualTimeline(const VirtualTimeline&) = delete;
    VirtualTimeline& operator=(const VirtualTimeline&) = delete;
    VirtualTimeline(VirtualTimeline&&) noexcept = default;
    VirtualTimeline& operator=(VirtualTimeline&&) noexcept = default;

    // Schedule an event at a specified virtual timestamp
    auto schedule(market::NanoTs timestamp, TimelineEventPriority priority,
                  TimelineEventPayload payload) noexcept -> std::uint64_t;

    // Schedule a custom callback action at a specified virtual timestamp
    auto schedule_action(market::NanoTs timestamp, TimelineAction action) noexcept -> std::uint64_t;

    // Inspection
    [[nodiscard]] auto has_events() const noexcept -> bool;
    [[nodiscard]] auto event_count() const noexcept -> std::size_t;
    [[nodiscard]] auto peek_next_time() const noexcept -> std::optional<market::NanoTs>;

    // Execution: step processes exactly one event, advancing the EventClock
    auto step() -> bool;

    // Execution: run until target_ts (inclusive)
    auto run_until(market::NanoTs target_ts) -> std::size_t;

    // Execution: run all scheduled events until queue is empty
    auto run_all() -> std::size_t;

    // Event dispatcher callbacks
    void set_feed_handler(std::function<void(const L3Message&)> handler) {
        feed_handler_ = std::move(handler);
    }
    void set_venue_arrival_handler(std::function<void(const execution::OrderRequest&)> handler) {
        venue_handler_ = std::move(handler);
    }
    void set_fill_delivery_handler(std::function<void(const core::ExecutionReport&)> handler) {
        fill_delivery_handler_ = std::move(handler);
    }

    void clear() noexcept;

private:
    EventClock& clock_;
    std::priority_queue<TimelineEvent, std::vector<TimelineEvent>, std::greater<TimelineEvent>>
        queue_;
    std::uint64_t next_schedule_id_{1};

    std::function<void(const L3Message&)> feed_handler_;
    std::function<void(const execution::OrderRequest&)> venue_handler_;
    std::function<void(const core::ExecutionReport&)> fill_delivery_handler_;
};

}  // namespace quantengine::replay
