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

#include "quantengine/core/types.hpp"

#include <ostream>

namespace quantengine::core {

std::ostream& operator<<(std::ostream& os, Side side) {
    return os << to_string(side);
}

std::ostream& operator<<(std::ostream& os, OrderStatus status) {
    return os << to_string(status);
}

std::ostream& operator<<(std::ostream& os, OrderType type) {
    return os << to_string(type);
}

std::ostream& operator<<(std::ostream& os, RejectReason reason) {
    return os << to_string(reason);
}

}  // namespace quantengine::core
