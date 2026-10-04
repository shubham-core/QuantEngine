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

// quantengine/replay/L3Message.hpp
//
// Canonical L3 (order-level) market data event types for historical feed replay.
//
// DESIGN RULES:
//   1. Every struct is trivially copyable — no std::string, no heap-owning members.
//   2. All prices use core::PriceTicks (int64_t fixed-point) — never float/double.
//   3. Timestamps are market::NanoTs (int64_t nanoseconds since Unix epoch).
//   4. VenueOrderId (uint64_t) is the exchange-assigned order reference number.
//      It is DISTINCT from core::OrderId (strategy-assigned; lives in the matching
//      engine). This distinction is critical: strategy orders never carry venue IDs,
//      and historical venue IDs must never be used as engine OrderIds.
//   5. Every struct has a compile-time sizeof assertion and trivially_copyable check.
//   6. Symbols use market::SymbolArray (fixed 16-byte null-padded char array).
//
// MESSAGE TAXONOMY (mirrors NASDAQ ITCH 5.0 order-level message set):
//   OrderAdded    — new passive limit order entered the exchange order book
//   OrderExecuted — resting order partially or fully filled by an aggressor
//   OrderCancelled— resting order removed or its quantity reduced
//   OrderReplaced — cancel-replace: old order removed, new order at new terms
//   TradeMessage  — uncorrelated trade print (off-book, odd-lot, cross, etc.)
//
// THREAD SAFETY:
//   All structs are value types. Copies are thread-safe by definition.
//   L3Message (the variant) is not synchronized — the replay layer must drive
//   it from a single thread.
//
// DETERMINISM:
//   These structs contain no pointers, no padding with undefined contents
//   (all pad fields are explicitly zero-initialized), and no virtual dispatch.
//   Two independently parsed messages from identical byte streams are equal.

#include <array>
#include <cstdint>
#include <string_view>
#include <type_traits>
#include <variant>

#include "quantengine/core/types.hpp"
#include "quantengine/market/MarketEvent.hpp"

namespace quantengine::replay {

// ---------------------------------------------------------------------------
// Type aliases for exchange-assigned identifiers.
// Deliberately NOT core::OrderId to prevent conflation with strategy-assigned
// client order IDs used by the matching engine and gateway layer.
// ---------------------------------------------------------------------------
using VenueOrderId = std::uint64_t;  // exchange-assigned order reference number
using MatchNumber = std::uint64_t;   // exchange-assigned trade/match reference number

// ---------------------------------------------------------------------------
// OrderAdded: a new passive limit order entered the exchange order book.
//
// The order sits at the BACK of the FIFO queue for (symbol, price, side).
// Queue position is: sum of all quantities of orders at the same price
// that entered before this one (earlier exchange_ts, or same ts + lower
// venue_order_id, per exchange-specific tie-breaking rules).
//
// 56 bytes — cache-line-friendly; matches core::Order(56B).
// ---------------------------------------------------------------------------
struct OrderAdded {
    market::NanoTs exchange_ts{0};     // nanoseconds since Unix epoch
    VenueOrderId venue_order_id{0};    // exchange-assigned reference (> 0)
    market::SymbolArray symbol{};      // 16-byte null-padded ticker
    core::PriceTicks price{0};         // fixed-point limit price (must be > 0)
    core::Quantity quantity{0};        // order quantity (must be > 0)
    core::Side side{core::Side::Buy};  // bid or ask
    std::uint8_t pad[7]{};             // explicit zero padding to 8-byte boundary

    [[nodiscard]] constexpr auto operator==(const OrderAdded&) const noexcept -> bool = default;
};
static_assert(sizeof(OrderAdded) == 56,
              "OrderAdded layout changed — verify padding and update docs");
static_assert(alignof(OrderAdded) == 8);
static_assert(std::is_trivially_copyable_v<OrderAdded>);

// ---------------------------------------------------------------------------
// OrderExecuted: exchange reports a fill against a resting order.
//
// executed_qty may be less than the resting quantity (partial fill).
// When the remaining quantity reaches zero, the order leaves the book.
// The fill price is the resting order's limit price (maker price rule).
// match_number uniquely identifies the crossing event at the exchange.
//
// 48 bytes.
// ---------------------------------------------------------------------------
struct OrderExecuted {
    market::NanoTs exchange_ts{0};
    VenueOrderId venue_order_id{0};  // resting order that was executed
    market::SymbolArray symbol{};
    core::Quantity executed_qty{0};  // quantity executed this event (must be > 0)
    MatchNumber match_number{0};     // exchange trade reference number

    [[nodiscard]] constexpr auto operator==(const OrderExecuted&) const noexcept -> bool = default;
};
static_assert(sizeof(OrderExecuted) == 48, "OrderExecuted layout changed — update docs");
static_assert(alignof(OrderExecuted) == 8);
static_assert(std::is_trivially_copyable_v<OrderExecuted>);

// ---------------------------------------------------------------------------
// OrderCancelled: a resting order was removed or its quantity was reduced.
//
// cancelled_qty specifies how much quantity was removed from the order.
// This may be a partial cancel (reduce) or a full cancel (remove entire order).
// The replay engine must track remaining quantity to know when the order
// fully leaves the book.
//
// 40 bytes.
// ---------------------------------------------------------------------------
struct OrderCancelled {
    market::NanoTs exchange_ts{0};
    VenueOrderId venue_order_id{0};  // order being (partially) cancelled
    market::SymbolArray symbol{};
    core::Quantity cancelled_qty{0};  // quantity removed (must be > 0)

    [[nodiscard]] constexpr auto operator==(const OrderCancelled&) const noexcept -> bool = default;
};
static_assert(sizeof(OrderCancelled) == 40, "OrderCancelled layout changed — update docs");
static_assert(alignof(OrderCancelled) == 8);
static_assert(std::is_trivially_copyable_v<OrderCancelled>);

// ---------------------------------------------------------------------------
// OrderReplaced: cancel-replace operation.
//
// The old order is atomically removed and a new order is inserted at the BACK
// of the new price level's FIFO queue — losing all queue priority.
// old_venue_order_id != new_venue_order_id is required (a self-replace is
// nonsensical and rejected by the parser).
//
// 64 bytes — exactly one 64-byte cache line.
// ---------------------------------------------------------------------------
struct OrderReplaced {
    market::NanoTs exchange_ts{0};
    VenueOrderId old_venue_order_id{0};  // order being replaced (removed)
    VenueOrderId new_venue_order_id{0};  // replacement order reference (> 0)
    market::SymbolArray symbol{};
    core::PriceTicks new_price{0};   // new limit price (must be > 0)
    core::Quantity new_quantity{0};  // new quantity (must be > 0)
    core::Side side{core::Side::Buy};
    std::uint8_t pad[7]{};

    [[nodiscard]] constexpr auto operator==(const OrderReplaced&) const noexcept -> bool = default;
};
static_assert(sizeof(OrderReplaced) == 64, "OrderReplaced layout changed — update docs");
static_assert(alignof(OrderReplaced) == 8);
static_assert(std::is_trivially_copyable_v<OrderReplaced>);

// ---------------------------------------------------------------------------
// TradeMessage: an uncorrelated trade print.
//
// This represents a trade that is not directly traceable to a specific resting
// order reference in the L3 book (e.g. off-book crosses, odd-lot prints, or
// exchange-reported trade corrections). Use OrderExecuted for book-level fills.
//
// conditions: exchange trade condition bitmask (0 = standard regular-way print).
//
// 56 bytes.
// ---------------------------------------------------------------------------
struct TradeMessage {
    market::NanoTs exchange_ts{0};
    MatchNumber match_number{0};  // exchange trade reference
    market::SymbolArray symbol{};
    core::PriceTicks price{0};              // execution price (must be > 0)
    core::Quantity quantity{0};             // trade quantity (must be > 0)
    core::Side aggressor{core::Side::Buy};  // which side was the taker
    std::uint8_t conditions{0};             // exchange trade condition bitmask
    std::uint8_t pad[6]{};

    [[nodiscard]] constexpr auto operator==(const TradeMessage&) const noexcept -> bool = default;
};
static_assert(sizeof(TradeMessage) == 56, "TradeMessage layout changed — update docs");
static_assert(alignof(TradeMessage) == 8);
static_assert(std::is_trivially_copyable_v<TradeMessage>);

// ---------------------------------------------------------------------------
// L3Message: discriminated union over all L3 event types.
//
// Variant index order must stay in sync with L3MessageKind enum below.
// OrderAdded at index 0 — highest frequency message type in typical ITCH feeds.
// ---------------------------------------------------------------------------
using L3Message =
    std::variant<OrderAdded, OrderExecuted, OrderCancelled, OrderReplaced, TradeMessage>;

enum class L3MessageKind : std::uint8_t {
    OrderAdded = 0,  // must match variant index
    OrderExecuted = 1,
    OrderCancelled = 2,
    OrderReplaced = 3,
    TradeMessage = 4,
};

[[nodiscard]] constexpr auto message_kind(const L3Message& msg) noexcept -> L3MessageKind {
    return static_cast<L3MessageKind>(msg.index());
}

[[nodiscard]] constexpr auto to_string(L3MessageKind kind) noexcept -> std::string_view {
    switch (kind) {
        case L3MessageKind::OrderAdded:
            return "ORDER_ADDED";
        case L3MessageKind::OrderExecuted:
            return "ORDER_EXECUTED";
        case L3MessageKind::OrderCancelled:
            return "ORDER_CANCELLED";
        case L3MessageKind::OrderReplaced:
            return "ORDER_REPLACED";
        case L3MessageKind::TradeMessage:
            return "TRADE_MESSAGE";
    }
    return "UNKNOWN";
}

// Extract the exchange timestamp from any L3Message without a full visit.
[[nodiscard]] inline auto l3_exchange_ts(const L3Message& msg) noexcept -> market::NanoTs {
    return std::visit([](const auto& m) noexcept -> market::NanoTs { return m.exchange_ts; }, msg);
}

// Extract the symbol string_view from any L3Message.
[[nodiscard]] inline auto l3_symbol(const L3Message& msg) noexcept -> std::string_view {
    return std::visit(
        [](const auto& m) noexcept -> std::string_view { return market::symbol_view(m.symbol); },
        msg);
}

}  // namespace quantengine::replay
