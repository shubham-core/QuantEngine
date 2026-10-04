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

#include <string>
#include <vector>

#include "quantengine/core/events.hpp"
#include "quantengine/engine/canonical_state.hpp"
#include "quantengine/engine/generic_matching_engine.hpp"
#include "quantengine/engine/invariants.hpp"

namespace quantengine::engine {

struct ReplayResult {
    CanonicalState::Hash final_hash{0};
    std::vector<core::ExecutionReport> execution_reports{};
    std::vector<core::Trade> all_trades{};
    bool invariants_satisfied{true};
    std::string failure_reason{};
};

class ReplayEngine {
public:
    template <typename BookType = reference::ReferenceOrderBook>
    [[nodiscard]] static ReplayResult replay(const std::vector<core::OrderCommand>& commands) {
        GenericMatchingEngine<BookType> engine;
        ReplayResult result;
        result.execution_reports.reserve(commands.size());

        for (const auto& cmd : commands) {
            auto report = engine.process_command(cmd);
            for (const auto& trade : report.trades) {
                result.all_trades.push_back(trade);
            }
            result.execution_reports.push_back(std::move(report));

            auto audit = InvariantAuditor::audit(engine);
            if (!audit.ok) {
                result.invariants_satisfied = false;
                result.failure_reason = audit.error_message;
                result.final_hash = CanonicalState::compute_hash(engine);
                return result;
            }
        }

        result.final_hash = CanonicalState::compute_hash(engine);
        result.invariants_satisfied = true;
        return result;
    }

    template <typename BookType1 = reference::ReferenceOrderBook,
              typename BookType2 = reference::ReferenceOrderBook>
    [[nodiscard]] static bool verify_determinism(const std::vector<core::OrderCommand>& commands) {
        const auto run1 = replay<BookType1>(commands);
        const auto run2 = replay<BookType2>(commands);

        if (!run1.invariants_satisfied || !run2.invariants_satisfied) {
            return false;
        }

        if (run1.final_hash != run2.final_hash) {
            return false;
        }

        if (run1.execution_reports.size() != run2.execution_reports.size()) {
            return false;
        }

        for (std::size_t i = 0; i < run1.execution_reports.size(); ++i) {
            if (run1.execution_reports[i] != run2.execution_reports[i]) {
                return false;
            }
        }

        if (run1.all_trades.size() != run2.all_trades.size()) {
            return false;
        }

        for (std::size_t i = 0; i < run1.all_trades.size(); ++i) {
            if (run1.all_trades[i] != run2.all_trades[i]) {
                return false;
            }
        }

        return true;
    }
};

}  // namespace quantengine::engine
