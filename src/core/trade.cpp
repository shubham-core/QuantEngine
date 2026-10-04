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

#include "quantengine/core/trade.hpp"

#include <ostream>

namespace quantengine::core {

std::ostream& operator<<(std::ostream& os, const Trade& trade) {
    return os << "Trade{id=" << trade.trade_id << ", maker=" << trade.maker_order_id
              << ", taker=" << trade.taker_order_id << ", side=" << trade.maker_side
              << ", px=" << trade.price << ", qty=" << trade.quantity
              << ", seq=" << trade.sequence_number << "}";
}

}  // namespace quantengine::core
