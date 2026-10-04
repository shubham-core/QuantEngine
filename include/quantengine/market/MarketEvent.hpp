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

// quantengine/market/MarketEvent.hpp
//
// Zero-allocation market event types for the hot-path event loop.
//
// DESIGN RULES (enforced by .clang-tidy; violations are CI failures):
//   1. Every struct is trivially copyable — no std::string, no std::vector,
//      no heap-owning members. Symbols are fixed-length char arrays.
//   2. All price fields use PriceTicks (int64 fixed-point, 1 tick = 0.01 USD
//      unless the instrument's TickSize changes). Never use double/float.
//   3. Timestamps are nanoseconds since Unix epoch (int64). No std::chrono on
//      the hot path — chrono_cast incurs hidden cost on some platforms.
//   4. MarketEvent = std::variant over the four event types. std::variant
//      storage is stack-only; visiting it is a single indirect call.
//   5. kMaxSymbolLen = 16 covers all equity/futures ticker symbols.
//      Crypto pairs may need 20 chars — extend with a recompile, not runtime.
//
// THREAD SAFETY:
//   All structs are value types. Copies are thread-safe by definition.
//   The MarketEvent variant itself is not synchronized — the feed layer must
//   deliver events on a single thread or via a lock-free queue.
//
// LIFETIME:
//   MarketEvent values are self-contained. No pointers, no references.
//   Safe to store in ring buffers, pass across thread boundaries by value.

#include <array>
#include <cstdint>
#include <string_view>
#include <variant>

#include "quantengine/core/types.hpp"

namespace quantengine::market {

// ---------------------------------------------------------------------------
// Symbol: fixed-capacity, null-terminated char array.
// 16 bytes keeps the struct naturally aligned and fits in a single register
// pair on x86-64.  Symbols longer than kMaxSymbolLen - 1 characters are
// truncated at feed-parse time with a logged warning.
// ---------------------------------------------------------------------------
inline constexpr std::size_t kMaxSymbolLen = 16;
using SymbolArray = std::array<char, kMaxSymbolLen>;

// Construct a SymbolArray from a string_view. Truncates silently if sv is
// longer than kMaxSymbolLen - 1; always null-terminates.
[[nodiscard]] constexpr auto make_symbol(std::string_view sv) noexcept -> SymbolArray {
    SymbolArray arr{};
    const auto len = sv.size() < (kMaxSymbolLen - 1) ? sv.size() : (kMaxSymbolLen - 1);
    for (std::size_t i = 0; i < len; ++i) {
        arr[i] = sv[i];
    }
    arr[len] = '\0';
    return arr;
}

[[nodiscard]] constexpr auto symbol_view(const SymbolArray& arr) noexcept -> std::string_view {
    // std::string_view from null-terminated array — zero allocation
    std::size_t len = 0;
    while (len < kMaxSymbolLen && arr[len] != '\0') {
        ++len;
    }
    return {arr.data(), len};
}

// FNV-1a hash for SymbolArray — enables use as std::unordered_map key.
struct SymbolHash {
    [[nodiscard]] auto operator()(const SymbolArray& s) const noexcept -> std::size_t {
        std::size_t h = 14695981039346656037ULL;
        for (const char c : s) {
            h ^= static_cast<std::size_t>(static_cast<unsigned char>(c));
            h *= 1099511628211ULL;
        }
        return h;
    }
};

// ---------------------------------------------------------------------------
// Timestamp: nanoseconds since Unix epoch.
// ---------------------------------------------------------------------------
using NanoTs = std::int64_t;

// ---------------------------------------------------------------------------
// Tick: a single last-trade print (exchange time + price + size).
// Equivalent to a "time & sales" entry. 40 bytes.
// ---------------------------------------------------------------------------
struct Tick {
    NanoTs exchange_ts{0};                  // exchange-reported trade timestamp (ns)
    NanoTs recv_ts{0};                      // local receive timestamp (ns); set by feed adapter
    SymbolArray symbol{};                   // 16 bytes
    core::PriceTicks price{0};              // fixed-point price in ticks
    core::Quantity quantity{0};             // shares / contracts
    core::Side aggressor{core::Side::Buy};  // which side was the taker
    std::uint8_t pad[7]{};                  // explicit padding to 8-byte boundary

    [[nodiscard]] constexpr auto operator==(const Tick&) const noexcept -> bool = default;
};
static_assert(sizeof(Tick) == 56, "Tick layout changed — update docs");
static_assert(alignof(Tick) == 8);

// ---------------------------------------------------------------------------
// Quote: a best-bid/offer update (top-of-book change).
// Equivalent to NBBO or L1 update. 64 bytes.
// ---------------------------------------------------------------------------
struct Quote {
    NanoTs exchange_ts{0};
    NanoTs recv_ts{0};
    SymbolArray symbol{};
    core::PriceTicks bid_price{0};
    core::PriceTicks ask_price{0};
    core::Quantity bid_size{0};
    core::Quantity ask_size{0};

    [[nodiscard]] constexpr auto operator==(const Quote&) const noexcept -> bool = default;

    // Spread in ticks; negative value means crossed market (never valid after risk gate)
    [[nodiscard]] constexpr auto spread_ticks() const noexcept -> core::PriceTicks {
        return ask_price - bid_price;
    }

    [[nodiscard]] constexpr auto is_crossed() const noexcept -> bool {
        return bid_price >= ask_price;
    }
};
static_assert(sizeof(Quote) == 64, "Quote layout changed — update docs");
static_assert(alignof(Quote) == 8);

// ---------------------------------------------------------------------------
// MarketTrade: an exchange-reported trade (distinct from engine-generated
// Trade in core::Trade). Used for last-sale reference, position marking,
// and strategy signal generation. 56 bytes.
// ---------------------------------------------------------------------------
struct MarketTrade {
    NanoTs exchange_ts{0};
    NanoTs recv_ts{0};
    SymbolArray symbol{};
    core::PriceTicks price{0};
    core::Quantity quantity{0};
    core::Side aggressor{core::Side::Buy};
    std::uint8_t conditions{0};  // exchange trade condition flags (bitmask)
    std::uint8_t pad[6]{};

    [[nodiscard]] constexpr auto operator==(const MarketTrade&) const noexcept -> bool = default;
};
static_assert(sizeof(MarketTrade) == 56, "MarketTrade layout changed — update docs");
static_assert(alignof(MarketTrade) == 8);

// ---------------------------------------------------------------------------
// PriceLevel: one depth level (price + total size + order count).
// 24 bytes — matches reference::LevelInfo but lives in the market namespace.
// ---------------------------------------------------------------------------
struct PriceLevel {
    core::PriceTicks price{0};
    core::Quantity size{0};
    std::uint32_t order_count{0};
    std::uint32_t pad{0};

    [[nodiscard]] constexpr auto operator==(const PriceLevel&) const noexcept -> bool = default;
};
static_assert(sizeof(PriceLevel) == 24);

// ---------------------------------------------------------------------------
// OrderBookSnapshot: a fixed-depth L2 snapshot delivered by the feed.
//
// kMaxDepth = 10 levels per side. 10 * 24 * 2 = 480 bytes + header = 512 B.
// This is intentionally stack-allocatable. Do NOT increase kMaxDepth beyond
// 20 without profiling ring-buffer throughput (each snapshot = one ring slot).
//
// For full-book depth, use a separate deep-copy structure off the hot path.
// ---------------------------------------------------------------------------
inline constexpr std::size_t kMaxDepth = 10;

struct OrderBookSnapshot {
    NanoTs exchange_ts{0};
    NanoTs recv_ts{0};
    SymbolArray symbol{};
    std::uint8_t bid_count{0};  // number of valid entries in bids[]
    std::uint8_t ask_count{0};  // number of valid entries in asks[]
    std::uint8_t pad[6]{};
    std::array<PriceLevel, kMaxDepth> bids{};  // descending by price
    std::array<PriceLevel, kMaxDepth> asks{};  // ascending by price

    [[nodiscard]] constexpr auto operator==(const OrderBookSnapshot&) const noexcept -> bool =
                                                                                            default;

    [[nodiscard]] constexpr auto best_bid() const noexcept -> const PriceLevel* {
        return (bid_count > 0) ? &bids[0] : nullptr;
    }

    [[nodiscard]] constexpr auto best_ask() const noexcept -> const PriceLevel* {
        return (ask_count > 0) ? &asks[0] : nullptr;
    }

    [[nodiscard]] constexpr auto mid_price() const noexcept -> core::PriceTicks {
        if (bid_count == 0 || ask_count == 0) {
            return 0;
        }
        return (bids[0].price + asks[0].price) / 2;
    }
};
static_assert(sizeof(OrderBookSnapshot) == 520, "OrderBookSnapshot layout changed — update docs");

// ---------------------------------------------------------------------------
// MarketEvent: discriminated union over all event types.
//
// std::variant stores the value inline (no heap). Visiting costs one indirect
// branch — acceptable on the strategy dispatch path. The variant index doubles
// as a cheap event-type filter before the full visit.
//
// Ordering is intentional: Quote (index 0) is the most frequent event type on
// equities feeds; placing it first minimises the variant index check cost on
// the common case.
// ---------------------------------------------------------------------------
using MarketEvent = std::variant<Quote, Tick, MarketTrade, OrderBookSnapshot>;

// Convenience type-tag helpers — zero cost, constexpr.
enum class MarketEventKind : std::uint8_t {
    Quote = 0,  // must match variant index order
    Tick = 1,
    MarketTrade = 2,
    OrderBookSnapshot = 3
};

[[nodiscard]] constexpr auto event_kind(const MarketEvent& ev) noexcept -> MarketEventKind {
    return static_cast<MarketEventKind>(ev.index());
}

[[nodiscard]] constexpr auto to_string(MarketEventKind kind) noexcept -> std::string_view {
    switch (kind) {
        case MarketEventKind::Quote:
            return "QUOTE";
        case MarketEventKind::Tick:
            return "TICK";
        case MarketEventKind::MarketTrade:
            return "TRADE";
        case MarketEventKind::OrderBookSnapshot:
            return "SNAPSHOT";
    }
    return "UNKNOWN";
}

// Extract the symbol from any MarketEvent without a full visit.
// Uses std::visit internally but has a branch-free result path.
[[nodiscard]] inline auto event_symbol(const MarketEvent& ev) noexcept -> std::string_view {
    return std::visit(
        [](const auto& e) noexcept -> std::string_view { return symbol_view(e.symbol); }, ev);
}

// Extract the receive timestamp from any MarketEvent.
[[nodiscard]] inline auto event_recv_ts(const MarketEvent& ev) noexcept -> NanoTs {
    return std::visit([](const auto& e) noexcept -> NanoTs { return e.recv_ts; }, ev);
}

}  // namespace quantengine::market
