// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <algorithm>
#include "common/types.h"

namespace VideoCore {
constexpr u32 ResolveLayerCount(u32 source_layers, u32 source_base, u32 source_count,
                                u32 target_layers, u32 target_base, u32 target_count) {
    if (source_base >= source_layers || target_base >= target_layers) {
        return 0;
    }
    return std::min({source_count, target_count, source_layers - source_base,
                     target_layers - target_base});
}
} // namespace VideoCore
