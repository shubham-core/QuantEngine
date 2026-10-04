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

#include <string_view>

namespace quantengine::core {

[[nodiscard]] constexpr std::string_view get_version() noexcept {
    return "0.1.0";
}

[[nodiscard]] bool is_engine_ready() noexcept;

}  // namespace quantengine::core
