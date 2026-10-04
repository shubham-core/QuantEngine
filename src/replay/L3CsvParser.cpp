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

// quantengine/replay/L3CsvParser.cpp
//
// Implementation of the IL3MarketDataSource CSV parser.
// See L3CsvParser.hpp for format specification and validation rules.

#include "quantengine/replay/L3CsvParser.hpp"

#include <charconv>  // std::from_chars — no null termination required
#include <cstring>   // std::memcpy
#include <limits>    // std::numeric_limits

namespace quantengine::replay {

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

L3CsvParser::L3CsvParser(std::istream& stream, std::string_view name) noexcept : stream_(stream) {
    const std::size_t copy_len = (name.size() < kNameBufLen) ? name.size() : (kNameBufLen - 1);
    std::memcpy(name_buf_, name.data(), copy_len);
    name_buf_[copy_len] = '\0';
    // Reserve typical line capacity to avoid reallocations in the hot loop.
    line_buf_.reserve(256);
}

// ---------------------------------------------------------------------------
// IL3MarketDataSource
// ---------------------------------------------------------------------------

auto L3CsvParser::next(L3Message& out) noexcept -> bool {
    while (std::getline(stream_, line_buf_)) {
        ++line_number_;

        // Strip Windows-style CRLF so tests and files work identically.
        if (!line_buf_.empty() && line_buf_.back() == '\r') {
            line_buf_.pop_back();
        }

        const auto trimmed = trim(std::string_view{line_buf_});

        // Skip blank lines and comment lines (first non-space char == '#').
        if (trimmed.empty() || trimmed.front() == '#') {
            continue;
        }

        const ParseStatus status = parse_line(trimmed, out);
        if (status == ParseStatus::Ok) {
            ++messages_consumed_;
            return true;
        }

        // Bad line: record the error and continue to the next line.
        // This implements lenient parsing — the caller checks error_count()
        // and last_error() after the drain to assess data quality.
        ++error_count_;
        last_error_ = status;
    }

    exhausted_ = true;
    return false;
}

auto L3CsvParser::is_exhausted() const noexcept -> bool {
    return exhausted_;
}

auto L3CsvParser::messages_consumed() const noexcept -> std::uint64_t {
    return messages_consumed_;
}

auto L3CsvParser::source_name() const noexcept -> std::string_view {
    return std::string_view{name_buf_};
}

auto L3CsvParser::last_error() const noexcept -> ParseStatus {
    return last_error_;
}

auto L3CsvParser::error_count() const noexcept -> std::uint64_t {
    return error_count_;
}

auto L3CsvParser::current_line_number() const noexcept -> std::uint64_t {
    return line_number_;
}

// ---------------------------------------------------------------------------
// Top-level dispatcher
// ---------------------------------------------------------------------------

auto L3CsvParser::parse_line(std::string_view line, L3Message& out) noexcept -> ParseStatus {
    FieldArray fields{};
    const std::size_t n = split_fields(line, fields);

    if (n == 0)
        return ParseStatus::MalformedLine;

    // Type field must be exactly one character.
    if (fields[0].size() != 1)
        return ParseStatus::UnknownMessageType;

    switch (fields[0][0]) {
        case 'A':
            return parse_order_added(fields, n, out);
        case 'E':
            return parse_order_executed(fields, n, out);
        case 'C':
            return parse_order_cancelled(fields, n, out);
        case 'R':
            return parse_order_replaced(fields, n, out);
        case 'T':
            return parse_trade_message(fields, n, out);
        default:
            return ParseStatus::UnknownMessageType;
    }
}

// ---------------------------------------------------------------------------
// Per-type parsers
// Field layout: f[0]=type, f[1..n-1]=data fields (as documented in header)
// ---------------------------------------------------------------------------

// A,ts_ns,venue_id,symbol,price_ticks,quantity,side   → 7 fields
auto L3CsvParser::parse_order_added(const FieldArray& f, std::size_t n,
                                    L3Message& out) noexcept -> ParseStatus {
    if (n != 7)
        return ParseStatus::MalformedLine;

    OrderAdded msg{};

    if (!parse_i64(f[1], msg.exchange_ts))
        return ParseStatus::InvalidField;

    if (!parse_u64(f[2], msg.venue_order_id))
        return ParseStatus::InvalidField;
    if (msg.venue_order_id == 0)
        return ParseStatus::InvalidField;

    msg.symbol = market::make_symbol(f[3]);  // truncates safely if > 15 chars

    if (!parse_i64(f[4], msg.price))
        return ParseStatus::InvalidField;
    if (msg.price <= 0)
        return ParseStatus::InvalidField;

    if (!parse_u64(f[5], msg.quantity))
        return ParseStatus::InvalidField;
    if (msg.quantity == 0)
        return ParseStatus::InvalidField;

    if (!parse_side(f[6], msg.side))
        return ParseStatus::InvalidField;

    out = msg;
    return ParseStatus::Ok;
}

// E,ts_ns,venue_id,symbol,executed_qty,match_number   → 6 fields
auto L3CsvParser::parse_order_executed(const FieldArray& f, std::size_t n,
                                       L3Message& out) noexcept -> ParseStatus {
    if (n != 6)
        return ParseStatus::MalformedLine;

    OrderExecuted msg{};

    if (!parse_i64(f[1], msg.exchange_ts))
        return ParseStatus::InvalidField;

    if (!parse_u64(f[2], msg.venue_order_id))
        return ParseStatus::InvalidField;
    if (msg.venue_order_id == 0)
        return ParseStatus::InvalidField;

    msg.symbol = market::make_symbol(f[3]);

    if (!parse_u64(f[4], msg.executed_qty))
        return ParseStatus::InvalidField;
    if (msg.executed_qty == 0)
        return ParseStatus::InvalidField;

    if (!parse_u64(f[5], msg.match_number))
        return ParseStatus::InvalidField;
    // match_number == 0 is permissible (some venues use 0 for uncorrelated prints)

    out = msg;
    return ParseStatus::Ok;
}

// C,ts_ns,venue_id,symbol,cancelled_qty               → 5 fields
auto L3CsvParser::parse_order_cancelled(const FieldArray& f, std::size_t n,
                                        L3Message& out) noexcept -> ParseStatus {
    if (n != 5)
        return ParseStatus::MalformedLine;

    OrderCancelled msg{};

    if (!parse_i64(f[1], msg.exchange_ts))
        return ParseStatus::InvalidField;

    if (!parse_u64(f[2], msg.venue_order_id))
        return ParseStatus::InvalidField;
    if (msg.venue_order_id == 0)
        return ParseStatus::InvalidField;

    msg.symbol = market::make_symbol(f[3]);

    if (!parse_u64(f[4], msg.cancelled_qty))
        return ParseStatus::InvalidField;
    if (msg.cancelled_qty == 0)
        return ParseStatus::InvalidField;

    out = msg;
    return ParseStatus::Ok;
}

// R,ts_ns,old_venue_id,new_venue_id,symbol,new_price_ticks,new_qty,side  → 8 fields
auto L3CsvParser::parse_order_replaced(const FieldArray& f, std::size_t n,
                                       L3Message& out) noexcept -> ParseStatus {
    if (n != 8)
        return ParseStatus::MalformedLine;

    OrderReplaced msg{};

    if (!parse_i64(f[1], msg.exchange_ts))
        return ParseStatus::InvalidField;

    if (!parse_u64(f[2], msg.old_venue_order_id))
        return ParseStatus::InvalidField;
    if (msg.old_venue_order_id == 0)
        return ParseStatus::InvalidField;

    if (!parse_u64(f[3], msg.new_venue_order_id))
        return ParseStatus::InvalidField;
    if (msg.new_venue_order_id == 0)
        return ParseStatus::InvalidField;

    // Self-replace: the new order would have the same queue slot as the old.
    // This is semantically invalid — a replace must produce a new reference.
    if (msg.old_venue_order_id == msg.new_venue_order_id)
        return ParseStatus::InvalidField;

    msg.symbol = market::make_symbol(f[4]);

    if (!parse_i64(f[5], msg.new_price))
        return ParseStatus::InvalidField;
    if (msg.new_price <= 0)
        return ParseStatus::InvalidField;

    if (!parse_u64(f[6], msg.new_quantity))
        return ParseStatus::InvalidField;
    if (msg.new_quantity == 0)
        return ParseStatus::InvalidField;

    if (!parse_side(f[7], msg.side))
        return ParseStatus::InvalidField;

    out = msg;
    return ParseStatus::Ok;
}

// T,ts_ns,match_number,symbol,price_ticks,quantity,aggressor_side,conditions  → 8 fields
auto L3CsvParser::parse_trade_message(const FieldArray& f, std::size_t n,
                                      L3Message& out) noexcept -> ParseStatus {
    if (n != 8)
        return ParseStatus::MalformedLine;

    TradeMessage msg{};

    if (!parse_i64(f[1], msg.exchange_ts))
        return ParseStatus::InvalidField;

    if (!parse_u64(f[2], msg.match_number))
        return ParseStatus::InvalidField;
    // match_number == 0 is acceptable for odd-lot and uncorrelated prints

    msg.symbol = market::make_symbol(f[3]);

    if (!parse_i64(f[4], msg.price))
        return ParseStatus::InvalidField;
    if (msg.price <= 0)
        return ParseStatus::InvalidField;

    if (!parse_u64(f[5], msg.quantity))
        return ParseStatus::InvalidField;
    if (msg.quantity == 0)
        return ParseStatus::InvalidField;

    if (!parse_side(f[6], msg.aggressor))
        return ParseStatus::InvalidField;

    if (!parse_u8(f[7], msg.conditions))
        return ParseStatus::InvalidField;

    out = msg;
    return ParseStatus::Ok;
}

// ---------------------------------------------------------------------------
// Primitive field parsers
// All use std::from_chars — no null-termination required, fully noexcept.
// ---------------------------------------------------------------------------

auto L3CsvParser::parse_i64(std::string_view sv, std::int64_t& out) noexcept -> bool {
    if (sv.empty())
        return false;
    const auto [ptr, ec] = std::from_chars(sv.data(), sv.data() + sv.size(), out);
    return ec == std::errc{} && ptr == sv.data() + sv.size();
}

auto L3CsvParser::parse_u64(std::string_view sv, std::uint64_t& out) noexcept -> bool {
    if (sv.empty())
        return false;
    const auto [ptr, ec] = std::from_chars(sv.data(), sv.data() + sv.size(), out);
    return ec == std::errc{} && ptr == sv.data() + sv.size();
}

auto L3CsvParser::parse_u8(std::string_view sv, std::uint8_t& out) noexcept -> bool {
    if (sv.empty())
        return false;
    std::uint32_t tmp{0};
    const auto [ptr, ec] = std::from_chars(sv.data(), sv.data() + sv.size(), tmp);
    if (ec != std::errc{} || ptr != sv.data() + sv.size())
        return false;
    if (tmp > std::numeric_limits<std::uint8_t>::max())
        return false;
    out = static_cast<std::uint8_t>(tmp);
    return true;
}

auto L3CsvParser::parse_side(std::string_view sv, core::Side& out) noexcept -> bool {
    if (sv.size() != 1)
        return false;
    if (sv[0] == 'B') {
        out = core::Side::Buy;
        return true;
    }
    if (sv[0] == 'S') {
        out = core::Side::Sell;
        return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// Tokenizer: split 'line' by commas, storing string_views into the buffer.
// Fields are trimmed of leading/trailing spaces (handles "A, 1000, AAPL").
// Returns the number of fields found (capped at kMaxFields).
// ---------------------------------------------------------------------------

auto L3CsvParser::split_fields(std::string_view line, FieldArray& fields) noexcept -> std::size_t {
    std::size_t count = 0;
    std::size_t start = 0;

    for (std::size_t i = 0; i <= line.size() && count < kMaxFields; ++i) {
        if (i == line.size() || line[i] == ',') {
            fields[count++] = trim(line.substr(start, i - start));
            start = i + 1;
        }
    }
    return count;
}

auto L3CsvParser::trim(std::string_view sv) noexcept -> std::string_view {
    const auto s = sv.find_first_not_of(' ');
    if (s == std::string_view::npos)
        return {};
    const auto e = sv.find_last_not_of(' ');
    return sv.substr(s, e - s + 1);
}

}  // namespace quantengine::replay
