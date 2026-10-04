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

#include <iosfwd>

#include "quantengine/core/types.hpp"

namespace quantengine::core {

struct Trade {
    TradeId trade_id{0};
    OrderId maker_order_id{0};
    OrderId taker_order_id{0};
    Side maker_side{Side::Sell};
    PriceTicks price{0};
    Quantity quantity{0};
    SeqNum sequence_number{0};

    [[nodiscard]] constexpr bool operator==(const Trade& other) const noexcept = default;
};

std::ostream& operator<<(std::ostream& os, const Trade& trade);

}  // namespace quantengine::core
