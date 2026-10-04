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

// src/journal/AsyncJournal.cpp
//
// M14 AsyncJournal implementation.

#include "quantengine/journal/AsyncJournal.hpp"

namespace quantengine::journal {

AsyncJournal::AsyncJournal(const AsyncJournalConfig& config) noexcept : config_(config) {}

AsyncJournal::~AsyncJournal() {
    stop();
}

bool AsyncJournal::start(std::string_view file_path, bool truncate) {
    if (running_.load(std::memory_order_acquire)) {
        return false;
    }

    if (!writer_.open(file_path, truncate)) {
        return false;
    }

    running_.store(true, std::memory_order_release);
    worker_thread_ = std::thread(&AsyncJournal::worker_loop, this);
    return true;
}

void AsyncJournal::stop() {
    if (!running_.exchange(false, std::memory_order_acq_rel)) {
        return;
    }

    cv_.notify_all();
    if (worker_thread_.joinable()) {
        worker_thread_.join();
    }

    writer_.close();
}

bool AsyncJournal::log_command(const core::OrderCommand& cmd, std::int64_t timestamp_ns) noexcept {
    if (!running_.load(std::memory_order_acquire)) {
        return false;
    }

    {
        const std::lock_guard<std::mutex> lock(queue_mutex_);
        if (queue_.size() >= config_.max_queue_capacity) {
            ++records_dropped_;
            return false;
        }

        JournalItem item{};
        item.kind = JournalItem::Kind::Command;
        item.timestamp_ns = timestamp_ns;
        item.payload = cmd;
        queue_.push(std::move(item));
        ++records_queued_;
    }

    cv_.notify_one();
    return true;
}

bool AsyncJournal::log_event(const market::MarketEvent& event) noexcept {
    if (!running_.load(std::memory_order_acquire)) {
        return false;
    }

    {
        const std::lock_guard<std::mutex> lock(queue_mutex_);
        if (queue_.size() >= config_.max_queue_capacity) {
            ++records_dropped_;
            return false;
        }

        JournalItem item{};
        item.kind = JournalItem::Kind::Event;
        item.timestamp_ns = 0;
        item.payload = event;
        queue_.push(std::move(item));
        ++records_queued_;
    }

    cv_.notify_one();
    return true;
}

void AsyncJournal::worker_loop() {
    std::vector<JournalItem> batch;
    batch.reserve(512);

    while (running_.load(std::memory_order_acquire)) {
        {
            std::unique_lock<std::mutex> lock(queue_mutex_);
            cv_.wait(lock, [this] {
                return !queue_.empty() || !running_.load(std::memory_order_acquire);
            });

            while (!queue_.empty() && batch.size() < 512) {
                batch.push_back(std::move(queue_.front()));
                queue_.pop();
            }
        }

        if (!batch.empty()) {
            for (const auto& item : batch) {
                if (item.kind == JournalItem::Kind::Command) {
                    writer_.append_command(std::get<core::OrderCommand>(item.payload),
                                           item.timestamp_ns);
                } else if (item.kind == JournalItem::Kind::Event) {
                    writer_.append_market_event(std::get<market::MarketEvent>(item.payload));
                }
                ++records_flushed_;
            }
            writer_.flush();
            batch.clear();
        }
    }

    // Drain remaining if configured
    if (config_.flush_on_stop) {
        std::unique_lock<std::mutex> lock(queue_mutex_);
        while (!queue_.empty()) {
            const auto& item = queue_.front();
            if (item.kind == JournalItem::Kind::Command) {
                writer_.append_command(std::get<core::OrderCommand>(item.payload),
                                       item.timestamp_ns);
            } else if (item.kind == JournalItem::Kind::Event) {
                writer_.append_market_event(std::get<market::MarketEvent>(item.payload));
            }
            ++records_flushed_;
            queue_.pop();
        }
        writer_.flush();
    }
}

}  // namespace quantengine::journal
