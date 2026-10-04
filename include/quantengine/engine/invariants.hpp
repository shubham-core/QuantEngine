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

#include <optional>
#include <sstream>
#include <string>

#include "quantengine/core/types.hpp"
#include "quantengine/engine/generic_matching_engine.hpp"

namespace quantengine::engine {

class InvariantAuditor {
public:
    struct AuditResult {
        bool ok{true};
        std::string error_message{};

        [[nodiscard]] constexpr explicit operator bool() const noexcept { return ok; }
    };

    template <typename BookType>
    [[nodiscard]] static bool is_book_uncrossed(const BookType& book) noexcept {
        const auto best_bid = book.best_bid_price();
        const auto best_ask = book.best_ask_price();

        if (best_bid && best_ask) {
            return *best_bid < *best_ask;
        }
        return true;
    }

    template <typename BookType>
    [[nodiscard]] static bool are_volumes_consistent(const BookType& book) noexcept {
        core::Quantity sum_bids = 0;
        for (const auto& level : book.get_bids()) {
            sum_bids += level.total_quantity;
        }
        if (sum_bids != book.total_bid_volume()) {
            return false;
        }

        core::Quantity sum_asks = 0;
        for (const auto& level : book.get_asks()) {
            sum_asks += level.total_quantity;
        }
        if (sum_asks != book.total_ask_volume()) {
            return false;
        }

        return true;
    }

    template <typename BookType>
    [[nodiscard]] static AuditResult audit(const GenericMatchingEngine<BookType>& engine) {
        const auto& book = engine.book();

        if (!book.check_invariants()) {
            return AuditResult{false, "Book internal invariant check failed"};
        }

        if (!is_book_uncrossed(book)) {
            std::ostringstream oss;
            oss << "Book crossed invariant violated: best_bid=" << *book.best_bid_price()
                << " >= best_ask=" << *book.best_ask_price();
            return AuditResult{false, oss.str()};
        }

        if (!are_volumes_consistent(book)) {
            return AuditResult{false, "Book volume consistency invariant violated"};
        }

        return AuditResult{true, ""};
    }
};

}  // namespace quantengine::engine
