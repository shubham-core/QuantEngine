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

// quantengine/replay/L3CsvParser.hpp
//
// IL3MarketDataSource implementation backed by a line-oriented CSV stream.
//
// PURPOSE:
//   Provides a deterministic, human-readable L3 feed for testing, CI
//   fixtures, and algorithm research. Not a production binary parser.
//   For ITCH 5.0 / PITCH binary feeds, implement a separate IL3MarketDataSource.
//
// CSV FORMAT (one message per non-comment, non-blank line):
//
//   A,<ts_ns>,<venue_id>,<symbol>,<price_ticks>,<quantity>,<side>
//   E,<ts_ns>,<venue_id>,<symbol>,<executed_qty>,<match_number>
//   C,<ts_ns>,<venue_id>,<symbol>,<cancelled_qty>
//   R,<ts_ns>,<old_venue_id>,<new_venue_id>,<symbol>,<new_price_ticks>,<new_qty>,<side>
//   T,<ts_ns>,<match_number>,<symbol>,<price_ticks>,<quantity>,<aggressor_side>,<conditions>
//
//   Lines starting with '#' and blank lines are silently skipped.
//   Leading/trailing spaces around field values are trimmed.
//   side / aggressor_side: 'B' = Buy, 'S' = Sell
//   conditions: uint8 bitmask (0 = standard print)
//
// PER-FIELD VALIDATION (parser enforces; semantic validation left to replay engine):
//   venue_id == 0          → InvalidField  (0 is the "invalid" sentinel)
//   price_ticks <= 0       → InvalidField  (must be a positive fixed-point value)
//   any quantity == 0      → InvalidField  (zero-quantity events are semantically void)
//   wrong field count      → MalformedLine
//   non-numeric integer    → InvalidField
//   unknown type char      → UnknownMessageType
//   invalid side ('B'/'S') → InvalidField
//   old == new venue_id    → InvalidField  (self-replace is nonsensical)
//
// NOT VALIDATED HERE (checked by L3ReplayEngine in Phase 3):
//   Duplicate venue_order_ids, OrderExecuted for unknown order, timestamp
//   monotonicity, symbol consistency across events for the same venue ID.
//
// ALLOCATION:
//   One std::string for getline buffering — reused across all lines.
//   Source name stored in a fixed-size char array (64 bytes).
//   No per-field heap allocations; all field parsing uses std::string_view
//   sub-views into the live line buffer.
//
// USAGE:
//   // From a file:
//   std::ifstream f{"data.csv"};
//   L3CsvParser parser{f, "data.csv"};
//
//   // From an inline string (unit tests):
//   std::istringstream ss{"A,1000000000,1,AAPL,15000,100,B"};
//   L3CsvParser parser{ss, "test"};
//
//   L3Message msg;
//   while (parser.next(msg)) { /* process msg */ }
//   if (parser.error_count() > 0) { /* log quality warning */ }

#include <array>
#include <cstdint>
#include <istream>
#include <limits>
#include <string>
#include <string_view>

#include "quantengine/replay/IL3MarketDataSource.hpp"
#include "quantengine/replay/L3Message.hpp"

namespace quantengine::replay {

class L3CsvParser final : public IL3MarketDataSource {
public:
    // Structured reason for the most recent parse failure.
    // Matches the validation rules documented in the header above.
    enum class ParseStatus : std::uint8_t {
        Ok = 0,
        MalformedLine,       // field count incorrect for this message type
        UnknownMessageType,  // type char not in {A, E, C, R, T}
        InvalidField,        // field fails type, range, or constraint check
        Exhausted            // source exhausted; not an error condition
    };

    // stream: any std::istream. The caller owns the stream and must ensure
    //   it outlives this parser. Prefer std::ifstream for files and
    //   std::istringstream for unit tests.
    // name: human-readable label stored in source_name().
    explicit L3CsvParser(std::istream& stream, std::string_view name = "csv") noexcept;

    // Not copyable (holds a reference). Not movable (reference member).
    L3CsvParser(const L3CsvParser&) = delete;
    auto operator=(const L3CsvParser&) -> L3CsvParser& = delete;
    L3CsvParser(L3CsvParser&&) = delete;
    auto operator=(L3CsvParser&&) -> L3CsvParser& = delete;

    ~L3CsvParser() override = default;

    // ---- IL3MarketDataSource ------------------------------------------------

    // Advance to the next valid message. Skips malformed lines internally.
    [[nodiscard]] auto next(L3Message& out) noexcept -> bool override;

    [[nodiscard]] auto is_exhausted() const noexcept -> bool override;
    [[nodiscard]] auto messages_consumed() const noexcept -> std::uint64_t override;
    [[nodiscard]] auto source_name() const noexcept -> std::string_view override;

    // ---- Error diagnostics --------------------------------------------------

    // Status from the most recently skipped (invalid) line.
    // ParseStatus::Ok if no errors have occurred or if parser is freshly constructed.
    [[nodiscard]] auto last_error() const noexcept -> ParseStatus;

    // Cumulative count of lines that failed validation and were skipped.
    [[nodiscard]] auto error_count() const noexcept -> std::uint64_t;

    // 1-based line number of the most recently read line (including skipped lines).
    [[nodiscard]] auto current_line_number() const noexcept -> std::uint64_t;

private:
    // Maximum number of CSV fields we ever need to parse (type R and T need 8).
    static constexpr std::size_t kMaxFields = 10;
    using FieldArray = std::array<std::string_view, kMaxFields>;

    // Top-level dispatcher: split line → identify type → delegate.
    [[nodiscard]] auto parse_line(std::string_view line, L3Message& out) noexcept -> ParseStatus;

    // Per-type parsers. Receive the full FieldArray and field count n.
    // Field indices: [0] = type char, [1..] = data fields.
    [[nodiscard]] static auto parse_order_added(const FieldArray& f, std::size_t n,
                                                L3Message& out) noexcept -> ParseStatus;
    [[nodiscard]] static auto parse_order_executed(const FieldArray& f, std::size_t n,
                                                   L3Message& out) noexcept -> ParseStatus;
    [[nodiscard]] static auto parse_order_cancelled(const FieldArray& f, std::size_t n,
                                                    L3Message& out) noexcept -> ParseStatus;
    [[nodiscard]] static auto parse_order_replaced(const FieldArray& f, std::size_t n,
                                                   L3Message& out) noexcept -> ParseStatus;
    [[nodiscard]] static auto parse_trade_message(const FieldArray& f, std::size_t n,
                                                  L3Message& out) noexcept -> ParseStatus;

    // Primitive field parsers using std::from_chars (no null-termination needed).
    [[nodiscard]] static auto parse_i64(std::string_view sv, std::int64_t& out) noexcept -> bool;
    [[nodiscard]] static auto parse_u64(std::string_view sv, std::uint64_t& out) noexcept -> bool;
    [[nodiscard]] static auto parse_u8(std::string_view sv, std::uint8_t& out) noexcept -> bool;
    [[nodiscard]] static auto parse_side(std::string_view sv, core::Side& out) noexcept -> bool;

    // Tokenize 'line' by commas, storing views into the line buffer.
    // Returns the number of fields found (up to kMaxFields).
    [[nodiscard]] static auto split_fields(std::string_view line,
                                           FieldArray& fields) noexcept -> std::size_t;

    // Strip leading/trailing ASCII spaces from sv.
    [[nodiscard]] static auto trim(std::string_view sv) noexcept -> std::string_view;

    // State
    std::istream& stream_;

    static constexpr std::size_t kNameBufLen = 64;
    char name_buf_[kNameBufLen]{};  // fixed-size storage for source_name()

    std::string line_buf_;          // reused across getline calls (one alloc, reused)
    std::uint64_t line_number_{0};  // 1-based line counter
    std::uint64_t messages_consumed_{0};
    std::uint64_t error_count_{0};
    ParseStatus last_error_{ParseStatus::Ok};
    bool exhausted_{false};
};

[[nodiscard]] constexpr auto to_string(L3CsvParser::ParseStatus s) noexcept -> std::string_view {
    switch (s) {
        case L3CsvParser::ParseStatus::Ok:
            return "OK";
        case L3CsvParser::ParseStatus::MalformedLine:
            return "MALFORMED_LINE";
        case L3CsvParser::ParseStatus::UnknownMessageType:
            return "UNKNOWN_MESSAGE_TYPE";
        case L3CsvParser::ParseStatus::InvalidField:
            return "INVALID_FIELD";
        case L3CsvParser::ParseStatus::Exhausted:
            return "EXHAUSTED";
    }
    return "UNKNOWN";
}

}  // namespace quantengine::replay
