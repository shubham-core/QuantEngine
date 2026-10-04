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

// src/journal/BinaryJournal.cpp
//
// M5 BinaryJournal implementation.

#include "quantengine/journal/BinaryJournal.hpp"

#include <chrono>
#include <cstring>

namespace quantengine::journal {

// ---------------------------------------------------------------------------
// BinaryJournalWriter Implementation
// ---------------------------------------------------------------------------

BinaryJournalWriter::~BinaryJournalWriter() {
    close();
}

std::uint32_t BinaryJournalWriter::compute_checksum(const void* data, std::size_t len) noexcept {
    const auto* bytes = static_cast<const std::uint8_t*>(data);
    std::uint32_t hash = 2166136261U;
    for (std::size_t i = 0; i < len; ++i) {
        hash ^= bytes[i];
        hash *= 16777619U;
    }
    return hash;
}

bool BinaryJournalWriter::open(std::string_view file_path, bool truncate) {
    close();

    const auto mode =
        std::ios::binary | std::ios::out | (truncate ? std::ios::trunc : std::ios::app);
    file_.open(std::string(file_path), mode);
    if (!file_.is_open()) {
        return false;
    }

    record_count_ = 0;
    bytes_written_ = 0;

    if (truncate || file_.tellp() == 0) {
        JournalHeader hdr{};
        hdr.created_ts = std::chrono::duration_cast<std::chrono::nanoseconds>(
                             std::chrono::system_clock::now().time_since_epoch())
                             .count();

        file_.write(reinterpret_cast<const char*>(&hdr), sizeof(hdr));
        if (!file_.good()) {
            close();
            return false;
        }
        bytes_written_ += sizeof(hdr);
    }

    return true;
}

void BinaryJournalWriter::close() {
    if (file_.is_open()) {
        file_.flush();
        file_.close();
    }
}

void BinaryJournalWriter::flush() {
    if (file_.is_open()) {
        file_.flush();
    }
}

bool BinaryJournalWriter::write_record(RecordType type, const void* payload, std::uint16_t size,
                                       std::int64_t ts) {
    if (!file_.is_open()) {
        return false;
    }

    RecordEnvelope env{};
    env.record_type = type;
    env.payload_size = size;
    env.sequence = ++record_count_;
    env.timestamp_ns = ts;
    env.checksum = compute_checksum(payload, size);

    file_.write(reinterpret_cast<const char*>(&env), sizeof(env));
    file_.write(reinterpret_cast<const char*>(payload), size);

    if (!file_.good()) {
        return false;
    }

    bytes_written_ += sizeof(env) + size;
    return true;
}

bool BinaryJournalWriter::append_command(const core::OrderCommand& cmd, std::int64_t timestamp_ns) {
    OrderCommandPayload payload{};
    payload.sequence_number = cmd.sequence_number;

    std::visit(
        [&payload](const auto& p) {
            using T = std::decay_t<decltype(p)>;
            if constexpr (std::is_same_v<T, core::CreateOrderCommand>) {
                payload.command_type = 1;
                payload.order_id = p.order_id;
                payload.side = static_cast<std::uint8_t>(p.side);
                payload.price = p.price;
                payload.quantity = p.quantity;
            } else if constexpr (std::is_same_v<T, core::CancelOrderCommand>) {
                payload.command_type = 2;
                payload.order_id = p.order_id;
            } else if constexpr (std::is_same_v<T, core::ModifyOrderCommand>) {
                payload.command_type = 3;
                payload.order_id = p.order_id;
                payload.new_price = p.new_price;
                payload.new_quantity = p.new_quantity;
            }
        },
        cmd.payload);

    return write_record(RecordType::OrderCommand, &payload, sizeof(payload), timestamp_ns);
}

bool BinaryJournalWriter::append_market_event(const market::MarketEvent& event) {
    MarketEventPayload payload{};

    if (const auto* q = std::get_if<market::Quote>(&event)) {
        payload.event_kind = 1;
        payload.symbol = q->symbol;
        payload.bid_or_trade_price = q->bid_price;
        payload.bid_or_trade_qty = q->bid_size;
        payload.ask_price = q->ask_price;
        payload.ask_qty = q->ask_size;
        payload.exchange_ts = q->exchange_ts;
        payload.recv_ts = q->recv_ts;
    } else if (const auto* t = std::get_if<market::Tick>(&event)) {
        payload.event_kind = 2;
        payload.symbol = t->symbol;
        payload.bid_or_trade_price = t->price;
        payload.bid_or_trade_qty = t->quantity;
        payload.exchange_ts = t->exchange_ts;
        payload.recv_ts = t->recv_ts;
    } else {
        return false;
    }

    return write_record(RecordType::MarketEvent, &payload, sizeof(payload), payload.recv_ts);
}

// ---------------------------------------------------------------------------
// BinaryJournalReader Implementation
// ---------------------------------------------------------------------------

BinaryJournalReader::~BinaryJournalReader() {
    close();
}

bool BinaryJournalReader::open(std::string_view file_path) {
    close();

    file_.open(std::string(file_path), std::ios::binary | std::ios::in);
    if (!file_.is_open()) {
        return false;
    }

    file_.read(reinterpret_cast<char*>(&header_), sizeof(header_));
    if (!file_.good() || !header_.is_valid()) {
        close();
        return false;
    }

    return true;
}

void BinaryJournalReader::close() {
    if (file_.is_open()) {
        file_.close();
    }
}

std::vector<core::OrderCommand> BinaryJournalReader::read_all_commands() {
    std::vector<core::OrderCommand> commands;
    if (!file_.is_open()) {
        return commands;
    }

    file_.seekg(sizeof(JournalHeader), std::ios::beg);

    RecordEnvelope env{};
    while (file_.read(reinterpret_cast<char*>(&env), sizeof(env))) {
        if (env.record_type == RecordType::OrderCommand) {
            OrderCommandPayload payload{};
            if (env.payload_size == sizeof(payload)) {
                file_.read(reinterpret_cast<char*>(&payload), sizeof(payload));
                if (!file_.good()) {
                    break;
                }

                core::OrderCommand cmd{};
                cmd.sequence_number = payload.sequence_number;

                if (payload.command_type == 1) {
                    cmd.payload =
                        core::CreateOrderCommand{.order_id = payload.order_id,
                                                 .side = static_cast<core::Side>(payload.side),
                                                 .price = payload.price,
                                                 .quantity = payload.quantity};
                } else if (payload.command_type == 2) {
                    cmd.payload = core::CancelOrderCommand{.order_id = payload.order_id};
                } else if (payload.command_type == 3) {
                    cmd.payload = core::ModifyOrderCommand{.order_id = payload.order_id,
                                                           .new_price = payload.new_price,
                                                           .new_quantity = payload.new_quantity};
                }
                commands.push_back(cmd);
            } else {
                file_.seekg(env.payload_size, std::ios::cur);
            }
        } else {
            file_.seekg(env.payload_size, std::ios::cur);
        }
    }

    file_.clear();
    return commands;
}

std::vector<market::MarketEvent> BinaryJournalReader::read_all_market_events() {
    std::vector<market::MarketEvent> events;
    if (!file_.is_open()) {
        return events;
    }

    file_.seekg(sizeof(JournalHeader), std::ios::beg);

    RecordEnvelope env{};
    while (file_.read(reinterpret_cast<char*>(&env), sizeof(env))) {
        if (env.record_type == RecordType::MarketEvent) {
            MarketEventPayload payload{};
            if (env.payload_size == sizeof(payload)) {
                file_.read(reinterpret_cast<char*>(&payload), sizeof(payload));
                if (!file_.good()) {
                    break;
                }

                if (payload.event_kind == 1) {
                    market::Quote q{};
                    q.symbol = payload.symbol;
                    q.bid_price = payload.bid_or_trade_price;
                    q.bid_size = payload.bid_or_trade_qty;
                    q.ask_price = payload.ask_price;
                    q.ask_size = payload.ask_qty;
                    q.exchange_ts = payload.exchange_ts;
                    q.recv_ts = payload.recv_ts;
                    events.push_back(market::MarketEvent{q});
                } else if (payload.event_kind == 2) {
                    market::Tick t{};
                    t.symbol = payload.symbol;
                    t.price = payload.bid_or_trade_price;
                    t.quantity = payload.bid_or_trade_qty;
                    t.exchange_ts = payload.exchange_ts;
                    t.recv_ts = payload.recv_ts;
                    events.push_back(market::MarketEvent{t});
                }
            } else {
                file_.seekg(env.payload_size, std::ios::cur);
            }
        } else {
            file_.seekg(env.payload_size, std::ios::cur);
        }
    }

    file_.clear();
    return events;
}

}  // namespace quantengine::journal
