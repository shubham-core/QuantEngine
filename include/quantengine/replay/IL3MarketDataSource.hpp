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

// quantengine/replay/IL3MarketDataSource.hpp
//
// Abstract interface for L3 (order-level) market data sources.
//
// DESIGN — Pull vs Push:
//   IL3MarketDataSource uses a PULL model (caller calls next()).
//   This is the inverse of IMarketDataFeed's push/callback model and is
//   deliberate:
//
//   1. The L3ReplayEngine (Phase 3) owns the event clock. It decides WHEN
//      to advance virtual time. A push/callback model would invert this
//      control, forcing the parser to drive the engine rather than the engine
//      driving the parser.
//
//   2. Historical replay is inherently single-threaded. Pull removes all
//      synchronization complexity.
//
//   3. Pull enables `step_until(ts)` semantics: the engine pulls messages
//      up to a target timestamp and stops, giving callers fine-grained
//      control over replay position.
//
// PARSE ERROR HANDLING:
//   next() does NOT throw on malformed input. Implementations skip invalid
//   records internally (incrementing an error counter) and return the next
//   valid message. The caller checks error_count() / last_error_status()
//   after the drain to detect data quality issues.
//   This design separates "the replay is running" from "there were bad records"
//   and allows partial processing of real-world feeds with occasional corruption.
//
// THREAD SAFETY:
//   Not thread-safe. All calls must come from a single thread (the replay
//   engine's dispatch thread). No internal synchronisation.
//
// LIFETIME:
//   The concrete implementation may hold a reference to an external istream
//   or other resource. The caller owns the lifetime of that resource and must
//   ensure it outlives the IL3MarketDataSource object.

#include <cstdint>
#include <string_view>

#include "quantengine/replay/L3Message.hpp"

namespace quantengine::replay {

class IL3MarketDataSource {
public:
    IL3MarketDataSource() = default;
    virtual ~IL3MarketDataSource() = default;

    IL3MarketDataSource(const IL3MarketDataSource&) = delete;
    auto operator=(const IL3MarketDataSource&) -> IL3MarketDataSource& = delete;
    IL3MarketDataSource(IL3MarketDataSource&&) = default;
    auto operator=(IL3MarketDataSource&&) -> IL3MarketDataSource& = default;

    // Advance to the next valid L3 message. Fills 'out' on success.
    //
    // Returns true when a valid message was placed in 'out'.
    // Returns false when the source is exhausted (no more messages remain).
    //
    // Implementations MUST be noexcept — a throw here terminates the replay loop.
    // Implementations skip invalid/malformed records internally and track
    // them via error_count(). Callers do NOT need to handle per-record parse errors;
    // only the final error_count() check after drain is required.
    [[nodiscard]] virtual auto next(L3Message& out) noexcept -> bool = 0;

    // Returns true after the last call to next() returned false.
    // Guaranteed to return false before the first next() call.
    [[nodiscard]] virtual auto is_exhausted() const noexcept -> bool = 0;

    // Total number of successfully parsed and returned messages.
    // Incremented only on successful next() == true calls.
    // Does NOT include skipped error records.
    [[nodiscard]] virtual auto messages_consumed() const noexcept -> std::uint64_t = 0;

    // Human-readable label for this source (e.g. "aapl_20240101.csv").
    // Used for logging and error reporting. Never empty.
    [[nodiscard]] virtual auto source_name() const noexcept -> std::string_view = 0;
};

}  // namespace quantengine::replay
