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

#include <cstdint>
#include <cstring>
#include <iomanip>
#include <sstream>
#include <string>
#include <vector>

#include "quantengine/engine/generic_matching_engine.hpp"

namespace quantengine::engine {

class CanonicalState {
public:
    using Hash = std::uint64_t;

    // 64-bit FNV-1a algorithm
    [[nodiscard]] static Hash fnv1a_64(const std::uint8_t* data, std::size_t size) noexcept {
        Hash hash = 14695981039346656037ULL;
        for (std::size_t i = 0; i < size; ++i) {
            hash ^= data[i];
            hash *= 1099511628211ULL;
        }
        return hash;
    }

    template <typename BookType>
    [[nodiscard]] static std::vector<std::uint8_t> serialize(
        const GenericMatchingEngine<BookType>& engine) {
        std::vector<std::uint8_t> buffer;
        buffer.reserve(1024);

        append_u64(buffer, engine.current_sequence());
        append_u64(buffer, engine.current_trade_id());

        const auto& book = engine.book();

        // Bids: descending
        const auto bids = book.get_bids();
        append_u32(buffer, static_cast<std::uint32_t>(bids.size()));
        for (const auto& level : bids) {
            append_i64(buffer, level.price);
            append_u64(buffer, level.total_quantity);
            append_u32(buffer, static_cast<std::uint32_t>(level.order_count));
        }

        // Asks: ascending
        const auto asks = book.get_asks();
        append_u32(buffer, static_cast<std::uint32_t>(asks.size()));
        for (const auto& level : asks) {
            append_i64(buffer, level.price);
            append_u64(buffer, level.total_quantity);
            append_u32(buffer, static_cast<std::uint32_t>(level.order_count));
        }

        // P0.3 Upgrade: Order-level granularity serialization
        // Bid orders in strict FIFO priority order per level
        const auto bid_orders = book.get_all_bid_orders();
        append_u32(buffer, static_cast<std::uint32_t>(bid_orders.size()));
        for (const auto& ord : bid_orders) {
            append_u64(buffer, ord.order_id());
            buffer.push_back(static_cast<std::uint8_t>(ord.side()));
            append_i64(buffer, ord.price());
            append_u64(buffer, ord.remaining_quantity());
            append_u64(buffer, ord.sequence_number());
        }

        // Ask orders in strict FIFO priority order per level
        const auto ask_orders = book.get_all_ask_orders();
        append_u32(buffer, static_cast<std::uint32_t>(ask_orders.size()));
        for (const auto& ord : ask_orders) {
            append_u64(buffer, ord.order_id());
            buffer.push_back(static_cast<std::uint8_t>(ord.side()));
            append_i64(buffer, ord.price());
            append_u64(buffer, ord.remaining_quantity());
            append_u64(buffer, ord.sequence_number());
        }

        return buffer;
    }

    template <typename BookType>
    [[nodiscard]] static Hash compute_hash(const GenericMatchingEngine<BookType>& engine) {
        const auto serialized = serialize(engine);
        return fnv1a_64(serialized.data(), serialized.size());
    }

    template <typename BookType>
    [[nodiscard]] static std::string to_string(const GenericMatchingEngine<BookType>& engine) {
        std::ostringstream oss;
        oss << "CanonicalState{\n"
            << "  seq=" << engine.current_sequence() << "\n"
            << "  trade_id=" << engine.current_trade_id() << "\n";

        const auto& book = engine.book();
        oss << "  bids=[\n";
        for (const auto& level : book.get_bids()) {
            oss << "    Level{px=" << level.price << ", qty=" << level.total_quantity
                << ", orders=" << level.order_count << "}\n";
        }
        oss << "  ]\n";

        oss << "  asks=[\n";
        for (const auto& level : book.get_asks()) {
            oss << "    Level{px=" << level.price << ", qty=" << level.total_quantity
                << ", orders=" << level.order_count << "}\n";
        }
        oss << "  ]\n"
            << "  hash=0x" << std::hex << std::setfill('0') << std::setw(16) << compute_hash(engine)
            << "\n"
            << "}";
        return oss.str();
    }

private:
    static inline void append_u32(std::vector<std::uint8_t>& buffer, std::uint32_t val) {
        buffer.push_back(static_cast<std::uint8_t>(val & 0xFF));
        buffer.push_back(static_cast<std::uint8_t>((val >> 8) & 0xFF));
        buffer.push_back(static_cast<std::uint8_t>((val >> 16) & 0xFF));
        buffer.push_back(static_cast<std::uint8_t>((val >> 24) & 0xFF));
    }

    static inline void append_u64(std::vector<std::uint8_t>& buffer, std::uint64_t val) {
        for (int i = 0; i < 8; ++i) {
            buffer.push_back(static_cast<std::uint8_t>((val >> (i * 8)) & 0xFF));
        }
    }

    static inline void append_i64(std::vector<std::uint8_t>& buffer, std::int64_t val) {
        std::uint64_t uval;
        std::memcpy(&uval, &val, sizeof(uval));
        append_u64(buffer, uval);
    }
};

}  // namespace quantengine::engine
