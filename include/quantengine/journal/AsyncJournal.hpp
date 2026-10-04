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

// quantengine/journal/AsyncJournal.hpp
//
// M14: Asynchronous non-blocking journal and trade logger.
// Runs a dedicated background disk-flushing thread to isolate the trading
// hot path from filesystem latency and stalls.

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <queue>
#include <string_view>
#include <thread>
#include <variant>

#include "quantengine/core/events.hpp"
#include "quantengine/journal/BinaryJournal.hpp"
#include "quantengine/market/MarketEvent.hpp"

namespace quantengine::journal {

struct AsyncJournalConfig {
    std::size_t max_queue_capacity{65536};
    bool flush_on_stop{true};
};

class AsyncJournal {
public:
    explicit AsyncJournal(const AsyncJournalConfig& config = {}) noexcept;
    ~AsyncJournal();

    AsyncJournal(const AsyncJournal&) = delete;
    auto operator=(const AsyncJournal&) -> AsyncJournal& = delete;
    AsyncJournal(AsyncJournal&&) = delete;
    auto operator=(AsyncJournal&&) -> AsyncJournal& = delete;

    [[nodiscard]] bool start(std::string_view file_path, bool truncate = true);
    void stop();

    [[nodiscard]] bool is_running() const noexcept {
        return running_.load(std::memory_order_acquire);
    }

    // Non-blocking hot-path append methods:
    bool log_command(const core::OrderCommand& cmd, std::int64_t timestamp_ns = 0) noexcept;
    bool log_event(const market::MarketEvent& event) noexcept;

    // Diagnostics
    [[nodiscard]] std::uint64_t records_queued() const noexcept { return records_queued_; }
    [[nodiscard]] std::uint64_t records_flushed() const noexcept { return records_flushed_; }
    [[nodiscard]] std::uint64_t records_dropped() const noexcept { return records_dropped_; }

private:
    void worker_loop();

    struct JournalItem {
        enum class Kind : std::uint8_t { Command, Event };
        Kind kind{Kind::Command};
        std::int64_t timestamp_ns{0};
        std::variant<core::OrderCommand, market::MarketEvent> payload{};
    };

    AsyncJournalConfig config_{};
    BinaryJournalWriter writer_{};

    mutable std::mutex queue_mutex_;
    std::condition_variable cv_;
    std::queue<JournalItem> queue_;

    std::thread worker_thread_;
    std::atomic<bool> running_{false};

    std::uint64_t records_queued_{0};
    std::uint64_t records_flushed_{0};
    std::uint64_t records_dropped_{0};
};

}  // namespace quantengine::journal
