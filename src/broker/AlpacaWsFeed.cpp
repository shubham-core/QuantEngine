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

// src/broker/AlpacaWsFeed.cpp
//
// M9 & M12 AlpacaWsFeed implementation.

#include "quantengine/broker/AlpacaWsFeed.hpp"

#include <algorithm>
#include <charconv>

namespace quantengine::broker {

namespace {

// Helper: extract string value between quotes following "key":
std::string_view extract_string_field(std::string_view json, std::string_view key) {
    const auto key_pos = json.find(key);
    if (key_pos == std::string_view::npos) {
        return {};
    }

    const auto colon_pos = json.find(':', key_pos + key.size());
    if (colon_pos == std::string_view::npos) {
        return {};
    }

    const auto open_quote = json.find('"', colon_pos);
    if (open_quote == std::string_view::npos) {
        return {};
    }

    const auto close_quote = json.find('"', open_quote + 1);
    if (close_quote == std::string_view::npos) {
        return {};
    }

    return json.substr(open_quote + 1, close_quote - open_quote - 1);
}

// Helper: extract numeric value following "key":
// Converts floating-point decimal prices (e.g. 150.25) to PriceTicks (e.g. 15025 cents / ticks)
core::PriceTicks extract_price_field(std::string_view json, std::string_view key,
                                     double scale = 100.0) {
    const auto key_pos = json.find(key);
    if (key_pos == std::string_view::npos) {
        return 0;
    }

    const auto colon_pos = json.find(':', key_pos + key.size());
    if (colon_pos == std::string_view::npos) {
        return 0;
    }

    std::size_t start = colon_pos + 1;
    while (start < json.size() && (json[start] == ' ' || json[start] == '\t')) {
        ++start;
    }

    std::size_t end = start;
    while (end < json.size() && (std::isdigit(static_cast<unsigned char>(json[end])) ||
                                 json[end] == '.' || json[end] == '-')) {
        ++end;
    }

    if (start == end) {
        return 0;
    }

    const auto num_str = json.substr(start, end - start);
    double val = 0.0;
    // Fast parse
    val = std::strtod(std::string(num_str).c_str(), nullptr);
    return static_cast<core::PriceTicks>(val * scale + (val >= 0 ? 0.5 : -0.5));
}

core::Quantity extract_quantity_field(std::string_view json, std::string_view key) {
    const auto key_pos = json.find(key);
    if (key_pos == std::string_view::npos) {
        return 0;
    }

    const auto colon_pos = json.find(':', key_pos + key.size());
    if (colon_pos == std::string_view::npos) {
        return 0;
    }

    std::size_t start = colon_pos + 1;
    while (start < json.size() && (json[start] == ' ' || json[start] == '\t')) {
        ++start;
    }

    std::size_t end = start;
    while (end < json.size() && std::isdigit(static_cast<unsigned char>(json[end]))) {
        ++end;
    }

    if (start == end) {
        return 0;
    }

    std::uint64_t val = 0;
    std::from_chars(json.data() + start, json.data() + end, val);
    return static_cast<core::Quantity>(val);
}

std::uint64_t extract_u64_field(std::string_view json, std::string_view key) {
    const auto key_pos = json.find(key);
    if (key_pos == std::string_view::npos) {
        return 0;
    }

    const auto colon_pos = json.find(':', key_pos + key.size());
    if (colon_pos == std::string_view::npos) {
        return 0;
    }

    std::size_t start = colon_pos + 1;
    while (start < json.size() && (json[start] == ' ' || json[start] == '\t')) {
        ++start;
    }

    std::size_t end = start;
    while (end < json.size() && std::isdigit(static_cast<unsigned char>(json[end]))) {
        ++end;
    }

    if (start == end) {
        return 0;
    }

    std::uint64_t val = 0;
    std::from_chars(json.data() + start, json.data() + end, val);
    return val;
}

}  // namespace

AlpacaWsFeed::AlpacaWsFeed(const AlpacaFeedConfig& config) noexcept : config_(config) {}

AlpacaWsFeed::~AlpacaWsFeed() {
    (void)stop();
}

market::FeedStatus AlpacaWsFeed::subscribe(market::EventHandlerBase& handler,
                                           std::string_view symbol,
                                           market::SubscriptionMask mask) noexcept {
    const std::lock_guard<std::mutex> lock(mutex_);
    subscriptions_.push_back(
        Subscription{.handler = &handler, .symbol = std::string(symbol), .mask = mask});
    return market::FeedStatus::Ok;
}

market::FeedStatus AlpacaWsFeed::unsubscribe(market::EventHandlerBase& handler,
                                             std::string_view symbol) noexcept {
    const std::lock_guard<std::mutex> lock(mutex_);
    subscriptions_.erase(std::remove_if(subscriptions_.begin(), subscriptions_.end(),
                                        [&](const Subscription& sub) {
                                            return sub.handler == &handler &&
                                                   (symbol.empty() || sub.symbol == symbol);
                                        }),
                         subscriptions_.end());
    return market::FeedStatus::Ok;
}

market::FeedStatus AlpacaWsFeed::start() noexcept {
    if (running_.exchange(true, std::memory_order_acq_rel)) {
        return market::FeedStatus::AlreadyRunning;
    }

    const std::lock_guard<std::mutex> lock(mutex_);
    for (auto& sub : subscriptions_) {
        if (sub.handler != nullptr) {
            sub.handler->on_connected();
        }
    }
    return market::FeedStatus::Ok;
}

market::FeedStatus AlpacaWsFeed::stop() noexcept {
    if (!running_.exchange(false, std::memory_order_acq_rel)) {
        return market::FeedStatus::NotRunning;
    }

    const std::lock_guard<std::mutex> lock(mutex_);
    for (auto& sub : subscriptions_) {
        if (sub.handler != nullptr) {
            sub.handler->on_disconnected();
        }
    }
    return market::FeedStatus::Ok;
}

bool AlpacaWsFeed::is_running() const noexcept {
    return running_.load(std::memory_order_acquire);
}

void AlpacaWsFeed::simulate_disconnect(std::string_view reason) noexcept {
    if (!running_.load(std::memory_order_acquire)) {
        return;
    }

    const std::lock_guard<std::mutex> lock(mutex_);
    for (auto& sub : subscriptions_) {
        if (sub.handler != nullptr) {
            sub.handler->on_error(reason);
            sub.handler->on_disconnected();
        }
    }
}

bool AlpacaWsFeed::reconnect() noexcept {
    if (!running_.load(std::memory_order_acquire)) {
        return false;
    }

    const std::lock_guard<std::mutex> lock(mutex_);
    ++reconnect_count_;
    for (auto& sub : subscriptions_) {
        if (sub.handler != nullptr) {
            sub.handler->on_connected();
        }
    }
    return true;
}

bool AlpacaWsFeed::process_raw_message(std::string_view json_text) noexcept {
    if (!running_.load(std::memory_order_acquire)) {
        return false;
    }

    ++messages_received_;

    // Handle JSON arrays e.g. [{"T":"q",...}, {"T":"t",...}]
    std::size_t pos = 0;
    while (pos < json_text.size()) {
        const auto start_obj = json_text.find('{', pos);
        if (start_obj == std::string_view::npos) {
            break;
        }

        const auto end_obj = json_text.find('}', start_obj);
        if (end_obj == std::string_view::npos) {
            break;
        }

        const auto obj_slice = json_text.substr(start_obj, end_obj - start_obj + 1);
        parse_and_dispatch_single(obj_slice);
        pos = end_obj + 1;
    }

    return true;
}

bool AlpacaWsFeed::parse_and_dispatch_single(std::string_view json_obj) noexcept {
    const auto msg_type = extract_string_field(json_obj, "\"T\"");
    const auto symbol_str = extract_string_field(json_obj, "\"S\"");
    if (symbol_str.empty()) {
        return false;
    }

    const auto symbol = market::make_symbol(symbol_str);

    // M12: Gap detection based on sequence number ("seq") if present
    if (config_.enable_gap_detection) {
        const auto seq = extract_u64_field(json_obj, "\"seq\"");
        if (seq > 0) {
            auto it = last_sequence_per_symbol_.find(std::string(symbol_str));
            if (it != last_sequence_per_symbol_.end()) {
                if (seq > it->second + 1) {
                    ++gaps_detected_;
                    // Notify error
                    const std::lock_guard<std::mutex> lock(mutex_);
                    for (auto& sub : subscriptions_) {
                        if (sub.handler != nullptr) {
                            sub.handler->on_error("SEQUENCE_GAP_DETECTED");
                        }
                    }
                }
            }
            last_sequence_per_symbol_[std::string(symbol_str)] = seq;
        }
    }

    if (msg_type == "q") {
        // Quote
        market::Quote quote{};
        quote.symbol = symbol;
        quote.bid_price = extract_price_field(json_obj, "\"bp\"");
        quote.ask_price = extract_price_field(json_obj, "\"ap\"");
        quote.bid_size = extract_quantity_field(json_obj, "\"bs\"");
        quote.ask_size = extract_quantity_field(json_obj, "\"as\"");
        quote.exchange_ts = static_cast<market::NanoTs>(extract_u64_field(json_obj, "\"t\""));
        quote.recv_ts = quote.exchange_ts;

        dispatch_event(market::MarketEvent{quote}, symbol);
        ++quotes_dispatched_;
        return true;
    } else if (msg_type == "t") {
        // Trade / Tick
        market::Tick tick{};
        tick.symbol = symbol;
        tick.price = extract_price_field(json_obj, "\"p\"");
        tick.quantity = extract_quantity_field(json_obj, "\"s\"");
        tick.exchange_ts = static_cast<market::NanoTs>(extract_u64_field(json_obj, "\"t\""));
        tick.recv_ts = tick.exchange_ts;

        dispatch_event(market::MarketEvent{tick}, symbol);
        ++trades_dispatched_;
        return true;
    }

    return false;
}

void AlpacaWsFeed::dispatch_event(const market::MarketEvent& event,
                                  const market::SymbolArray& symbol) noexcept {
    const std::string sym_str(market::symbol_view(symbol));

    const std::lock_guard<std::mutex> lock(mutex_);
    for (auto& sub : subscriptions_) {
        if (sub.handler == nullptr) {
            continue;
        }
        if (!sub.symbol.empty() && sub.symbol != sym_str) {
            continue;
        }

        sub.handler->on_event(event);
    }
}

}  // namespace quantengine::broker
