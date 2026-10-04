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

// quantengine/journal/BinaryJournal.hpp
//
// M5: Binary Event Journal.
// Append-only, schema-versioned file format for recording order commands,
// market events, and execution reports for deterministic replay and audit.

#include <array>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <string_view>
#include <vector>

#include "quantengine/core/events.hpp"
#include "quantengine/core/types.hpp"
#include "quantengine/engine/generic_matching_engine.hpp"
#include "quantengine/market/MarketEvent.hpp"

namespace quantengine::journal {

// ---------------------------------------------------------------------------
// File Header & Envelope Definitions
// ---------------------------------------------------------------------------

inline constexpr std::array<char, 4> kJournalMagic{'Q', 'E', 'V', 'J'};
inline constexpr std::uint32_t kJournalCurrentVersion = 1;

#pragma pack(push, 1)

struct JournalHeader {
    std::array<char, 4> magic{kJournalMagic};
    std::uint32_t version{kJournalCurrentVersion};
    std::int64_t created_ts{0};
    std::uint8_t reserved[16]{};

    [[nodiscard]] constexpr bool is_valid() const noexcept {
        return magic == kJournalMagic && version == kJournalCurrentVersion;
    }
};
static_assert(sizeof(JournalHeader) == 32);

enum class RecordType : std::uint16_t { OrderCommand = 1, MarketEvent = 2, ExecutionReport = 3 };

struct RecordEnvelope {
    RecordType record_type{RecordType::OrderCommand};
    std::uint16_t payload_size{0};
    std::uint64_t sequence{0};
    std::int64_t timestamp_ns{0};
    std::uint32_t checksum{0};  // FNV-1a checksum of the payload
    std::uint32_t pad{0};
};
static_assert(sizeof(RecordEnvelope) == 28);

// Payload for OrderCommand
struct OrderCommandPayload {
    std::uint64_t sequence_number{0};
    std::uint8_t command_type{0};  // 1 = Create, 2 = Cancel, 3 = Modify
    std::uint8_t side{0};          // 0 = Buy, 1 = Sell
    std::uint8_t pad[6]{};
    core::OrderId order_id{0};
    core::PriceTicks price{0};
    core::Quantity quantity{0};
    core::PriceTicks new_price{0};
    core::Quantity new_quantity{0};
};
static_assert(sizeof(OrderCommandPayload) == 56);

// Payload for MarketEvent (Quote & Tick supported in journal v1)
struct MarketEventPayload {
    std::uint8_t event_kind{0};  // 1 = Quote, 2 = Tick
    std::uint8_t pad[7]{};
    market::SymbolArray symbol{};
    core::PriceTicks bid_or_trade_price{0};
    core::PriceTicks ask_price{0};
    core::Quantity bid_or_trade_qty{0};
    core::Quantity ask_qty{0};
    std::int64_t exchange_ts{0};
    std::int64_t recv_ts{0};
};
static_assert(sizeof(MarketEventPayload) == 72);

#pragma pack(pop)

// ---------------------------------------------------------------------------
// BinaryJournalWriter: Append-only streaming writer
// ---------------------------------------------------------------------------
class BinaryJournalWriter {
public:
    BinaryJournalWriter() = default;
    ~BinaryJournalWriter();

    BinaryJournalWriter(const BinaryJournalWriter&) = delete;
    auto operator=(const BinaryJournalWriter&) -> BinaryJournalWriter& = delete;
    BinaryJournalWriter(BinaryJournalWriter&&) = default;
    auto operator=(BinaryJournalWriter&&) -> BinaryJournalWriter& = default;

    [[nodiscard]] bool open(std::string_view file_path, bool truncate = true);
    void close();
    void flush();

    [[nodiscard]] bool is_open() const noexcept { return file_.is_open(); }
    [[nodiscard]] std::uint64_t record_count() const noexcept { return record_count_; }
    [[nodiscard]] std::uint64_t bytes_written() const noexcept { return bytes_written_; }

    bool append_command(const core::OrderCommand& cmd, std::int64_t timestamp_ns = 0);
    bool append_market_event(const market::MarketEvent& event);

private:
    [[nodiscard]] static std::uint32_t compute_checksum(const void* data, std::size_t len) noexcept;
    bool write_record(RecordType type, const void* payload, std::uint16_t size, std::int64_t ts);

    std::ofstream file_;
    std::uint64_t record_count_{0};
    std::uint64_t bytes_written_{0};
};

// ---------------------------------------------------------------------------
// BinaryJournalReader: Replay and inspection reader
// ---------------------------------------------------------------------------
class BinaryJournalReader {
public:
    BinaryJournalReader() = default;
    ~BinaryJournalReader();

    BinaryJournalReader(const BinaryJournalReader&) = delete;
    auto operator=(const BinaryJournalReader&) -> BinaryJournalReader& = delete;
    BinaryJournalReader(BinaryJournalReader&&) = default;
    auto operator=(BinaryJournalReader&&) -> BinaryJournalReader& = default;

    [[nodiscard]] bool open(std::string_view file_path);
    void close();

    [[nodiscard]] bool is_open() const noexcept { return file_.is_open(); }
    [[nodiscard]] const JournalHeader& header() const noexcept { return header_; }

    [[nodiscard]] std::vector<core::OrderCommand> read_all_commands();
    [[nodiscard]] std::vector<market::MarketEvent> read_all_market_events();

    template <typename BookType>
    [[nodiscard]] std::size_t replay_into(engine::GenericMatchingEngine<BookType>& engine) {
        const auto commands = read_all_commands();
        for (const auto& cmd : commands) {
            (void)engine.process_command(cmd);
        }
        return commands.size();
    }

private:
    std::ifstream file_;
    JournalHeader header_{};
};

}  // namespace quantengine::journal
