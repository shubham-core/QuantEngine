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

// quantengine/market/IMarketDataFeed.hpp
//
// Abstract interface for market data sources.
//
// DESIGN RATIONALE:
//   Two competing designs were considered:
//
//   A) Callback/observer (push): feed calls handler(event) synchronously.
//      PRO: lowest latency, no queue overhead.
//      CON: handler runs on the feed thread — any blocking in the handler
//           (e.g. a slow strategy) applies backpressure to the feed socket.
//
//   B) Lock-free ring buffer (pull): feed writes to ring, strategy reads.
//      PRO: decoupled threads, strategy never stalls the feed.
//      CON: one extra memory copy, latency floor = ring poll interval.
//
//   DECISION: We use design A (push/callback) for the synchronous hot path.
//   The IEventHandler concept defines the callback contract. The ring buffer
//   decoupling belongs in the IMarketDataFeed *implementation* layer (e.g.
//   a lock-free SPSC queue between the socket thread and the strategy thread),
//   not in this interface. The interface stays minimal.
//
// THREAD SAFETY:
//   - IMarketDataFeed::subscribe() and unsubscribe() are called from the
//     main/setup thread BEFORE start(). NOT safe to call from the feed thread.
//   - IMarketDataFeed::start() / stop() may block or spawn threads internally.
//     The implementation must document its threading model.
//   - on_event() is called from the feed thread. The handler MUST be
//     re-entrant and MUST NOT block. Any slow work belongs in a queue.
//   - on_error() and on_disconnected() are also called from the feed thread.
//
// LIFETIME:
//   The IEventHandler pointer/reference passed to subscribe() must remain
//   valid until unsubscribe() is called or the feed is destroyed.
//   Dangling handler pointers are undefined behaviour — no reference counting
//   is provided intentionally (adds overhead; caller owns lifetime).

#include <cstdint>
#include <string_view>

#include "quantengine/market/MarketEvent.hpp"

namespace quantengine::market {

// ---------------------------------------------------------------------------
// FeedStatus: returned by start()/stop()/subscribe() to avoid exceptions on
// the setup path. Exceptions are banned on the hot path; this status type
// gives callers structured error information without dynamic allocation.
// ---------------------------------------------------------------------------
enum class FeedStatus : std::uint8_t {
    Ok = 0,
    AlreadyRunning,
    NotRunning,
    SymbolNotFound,
    ConnectionFailed,
    AuthenticationFailed,
    RateLimitExceeded,
    InternalError
};

[[nodiscard]] constexpr auto to_string(FeedStatus s) noexcept -> std::string_view {
    switch (s) {
        case FeedStatus::Ok:
            return "OK";
        case FeedStatus::AlreadyRunning:
            return "ALREADY_RUNNING";
        case FeedStatus::NotRunning:
            return "NOT_RUNNING";
        case FeedStatus::SymbolNotFound:
            return "SYMBOL_NOT_FOUND";
        case FeedStatus::ConnectionFailed:
            return "CONNECTION_FAILED";
        case FeedStatus::AuthenticationFailed:
            return "AUTH_FAILED";
        case FeedStatus::RateLimitExceeded:
            return "RATE_LIMITED";
        case FeedStatus::InternalError:
            return "INTERNAL_ERROR";
    }
    return "UNKNOWN";
}

// ---------------------------------------------------------------------------
// SubscriptionMask: bitmask controlling which event types are delivered to
// a subscriber. Unused bits cost nothing — the feed filters before dispatch.
// ---------------------------------------------------------------------------
enum class SubscriptionMask : std::uint8_t {
    None = 0b0000,
    Quotes = 0b0001,
    Ticks = 0b0010,
    Trades = 0b0100,
    Snapshots = 0b1000,
    All = 0b1111
};

[[nodiscard]] constexpr auto operator|(SubscriptionMask a,
                                       SubscriptionMask b) noexcept -> SubscriptionMask {
    return static_cast<SubscriptionMask>(static_cast<std::uint8_t>(a) |
                                         static_cast<std::uint8_t>(b));
}

[[nodiscard]] constexpr auto operator&(SubscriptionMask a,
                                       SubscriptionMask b) noexcept -> SubscriptionMask {
    return static_cast<SubscriptionMask>(static_cast<std::uint8_t>(a) &
                                         static_cast<std::uint8_t>(b));
}

[[nodiscard]] constexpr auto has_flag(SubscriptionMask mask,
                                      SubscriptionMask flag) noexcept -> bool {
    return (mask & flag) != SubscriptionMask::None;
}

// ---------------------------------------------------------------------------
// IEventHandler: C++20 concept constraining the callback object.
//
// Using a concept instead of a virtual base for the handler achieves two
// things:
//   1. The feed can call on_event() with zero virtual dispatch overhead when
//      the handler type is known at compile time (template-based feed).
//   2. Virtual-dispatch feed implementations can wrap any IEventHandler-
//      conforming type in a type-erased shim (see EventHandlerAdaptor below).
//
// Required interface (non-virtual):
//   void on_event(const MarketEvent&) noexcept;
//   void on_error(std::string_view reason) noexcept;
//   void on_disconnected() noexcept;
//   void on_connected() noexcept;
// ---------------------------------------------------------------------------
template <typename T>
concept IEventHandler = requires(T& handler, const MarketEvent& ev) {
    { handler.on_event(ev) } noexcept -> std::same_as<void>;
    { handler.on_error("") } noexcept -> std::same_as<void>;
    { handler.on_disconnected() } noexcept -> std::same_as<void>;
    { handler.on_connected() } noexcept -> std::same_as<void>;
};

// ---------------------------------------------------------------------------
// EventHandlerBase: optional virtual base for callers who want runtime
// polymorphism over handlers (e.g. fan-out to multiple strategies).
// Not required by IEventHandler concept — mix-and-match is fine.
// ---------------------------------------------------------------------------
class EventHandlerBase {
public:
    EventHandlerBase() = default;
    virtual ~EventHandlerBase() = default;

    EventHandlerBase(const EventHandlerBase&) = delete;
    auto operator=(const EventHandlerBase&) -> EventHandlerBase& = delete;
    EventHandlerBase(EventHandlerBase&&) = default;
    auto operator=(EventHandlerBase&&) -> EventHandlerBase& = default;

    // Called for every event passing the SubscriptionMask filter.
    // MUST be noexcept — throwing here terminates the feed thread.
    virtual void on_event(const MarketEvent& event) noexcept = 0;

    // Called when the connection is established and the feed is streaming.
    virtual void on_connected() noexcept = 0;

    // Called on clean disconnect (stop() called) or network error.
    // Implementations must distinguish via the reason string.
    virtual void on_disconnected() noexcept = 0;

    // Called on recoverable errors (e.g. sequence gap, malformed message).
    // The feed may continue running. Handler must not block.
    virtual void on_error(std::string_view reason) noexcept = 0;
};

// Verify EventHandlerBase satisfies its own concept (compile-time check).
static_assert(IEventHandler<EventHandlerBase>,
              "EventHandlerBase must satisfy the IEventHandler concept");

// ---------------------------------------------------------------------------
// IMarketDataFeed: abstract feed interface.
//
// Implementations: CsvReplayFeed (local deterministic testing),
//                  AlpacaWsFeed (live paper trading).
//
// CONTRACT:
//   subscribe() before start().
//   stop() before destroying the feed object.
//   Never call subscribe() / unsubscribe() after start() — thread-unsafe.
// ---------------------------------------------------------------------------
class IMarketDataFeed {
public:
    IMarketDataFeed() = default;
    virtual ~IMarketDataFeed() = default;

    IMarketDataFeed(const IMarketDataFeed&) = delete;
    auto operator=(const IMarketDataFeed&) -> IMarketDataFeed& = delete;
    IMarketDataFeed(IMarketDataFeed&&) = default;
    auto operator=(IMarketDataFeed&&) -> IMarketDataFeed& = default;

    // Register a handler for events on the given symbol.
    // mask controls which event types are delivered.
    // Returns SymbolNotFound if the feed cannot provide data for this symbol.
    // MUST be called before start(). NOT thread-safe.
    [[nodiscard]] virtual auto subscribe(EventHandlerBase& handler, std::string_view symbol,
                                         SubscriptionMask mask = SubscriptionMask::All) noexcept
        -> FeedStatus = 0;

    // Remove a previously registered handler for the given symbol.
    // No-op if the handler was not subscribed.
    // MUST be called before start() or after stop(). NOT thread-safe.
    [[nodiscard]] virtual auto unsubscribe(EventHandlerBase& handler,
                                           std::string_view symbol) noexcept -> FeedStatus = 0;

    // Start delivering events. May block until connected or return immediately
    // and drive events from a background thread — implementation-defined.
    // Returns AlreadyRunning if called twice.
    [[nodiscard]] virtual auto start() noexcept -> FeedStatus = 0;

    // Stop the feed gracefully. Flushes any in-flight events before returning.
    // Blocks until the internal thread (if any) has joined.
    // Returns NotRunning if called before start().
    [[nodiscard]] virtual auto stop() noexcept -> FeedStatus = 0;

    // Returns true between a successful start() and stop().
    [[nodiscard]] virtual auto is_running() const noexcept -> bool = 0;

    // Human-readable feed identifier for logging (e.g. "alpaca-ws", "csv-replay").
    [[nodiscard]] virtual auto feed_name() const noexcept -> std::string_view = 0;
};

}  // namespace quantengine::market
