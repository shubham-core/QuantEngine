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

#include "quantengine/engine/generic_matching_engine.hpp"

// Header alias forwarder for backwards compatibility
namespace quantengine::engine {

// MatchingEngine is an alias to GenericMatchingEngine<ReferenceOrderBook>
// OptimizedMatchingEngine is an alias to GenericMatchingEngine<OptimizedOrderBook>

}  // namespace quantengine::engine
