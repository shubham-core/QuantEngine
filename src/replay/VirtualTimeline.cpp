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

#include "quantengine/replay/VirtualTimeline.hpp"

namespace quantengine::replay {

auto VirtualTimeline::schedule(market::NanoTs timestamp, TimelineEventPriority priority,
                               TimelineEventPayload payload) noexcept -> std::uint64_t {
    const std::uint64_t id = next_schedule_id_++;
    queue_.push(TimelineEvent{
        .timestamp = timestamp,
        .priority = priority,
        .schedule_id = id,
        .payload = std::move(payload),
    });
    return id;
}

auto VirtualTimeline::schedule_action(market::NanoTs timestamp,
                                      TimelineAction action) noexcept -> std::uint64_t {
    return schedule(timestamp, TimelineEventPriority::Action, std::move(action));
}

auto VirtualTimeline::has_events() const noexcept -> bool {
    return !queue_.empty();
}

auto VirtualTimeline::event_count() const noexcept -> std::size_t {
    return queue_.size();
}

auto VirtualTimeline::peek_next_time() const noexcept -> std::optional<market::NanoTs> {
    if (queue_.empty())
        return std::nullopt;
    return queue_.top().timestamp;
}

auto VirtualTimeline::step() -> bool {
    if (queue_.empty())
        return false;

    TimelineEvent event = std::move(const_cast<TimelineEvent&>(queue_.top()));
    queue_.pop();

    // Advance clock to this event's timestamp
    clock_.advance_to(event.timestamp);

    // Dispatch payload
    std::visit(
        [this](auto&& p) {
            using T = std::decay_t<decltype(p)>;
            if constexpr (std::is_same_v<T, FeedEventPayload>) {
                if (feed_handler_)
                    feed_handler_(p.message);
            } else if constexpr (std::is_same_v<T, VenueArrivalPayload>) {
                if (venue_handler_)
                    venue_handler_(p.request);
            } else if constexpr (std::is_same_v<T, FillDeliveryPayload>) {
                if (fill_delivery_handler_)
                    fill_delivery_handler_(p.report);
            } else if constexpr (std::is_same_v<T, TimelineAction>) {
                if (p)
                    p(clock_);
            }
        },
        event.payload);

    return true;
}

auto VirtualTimeline::run_until(market::NanoTs target_ts) -> std::size_t {
    std::size_t processed = 0;
    while (!queue_.empty() && queue_.top().timestamp <= target_ts) {
        step();
        ++processed;
    }
    // Ensure clock advances to target_ts if it was beyond all processed events
    clock_.advance_to(target_ts);
    return processed;
}

auto VirtualTimeline::run_all() -> std::size_t {
    std::size_t processed = 0;
    while (!queue_.empty()) {
        step();
        ++processed;
    }
    return processed;
}

void VirtualTimeline::clear() noexcept {
    while (!queue_.empty()) {
        queue_.pop();
    }
}

}  // namespace quantengine::replay
