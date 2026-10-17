// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstring>
#include "core/libraries/vr_tracker/vr_tracker.h"

namespace Libraries::VrTracker {
// Statik's caller (CUSA06929, RVA 0x27dd98a) supplies a 0x90-byte layout:
// calibration at 0x20, buffers at 0x40/0x50/0x60 and queue at 0x70.
// The existing 0x80-byte ABI has an extra cpu_mask before calibration.
// Normalize only fields consumed by this HLE; trailing/unknown fields stay unused.
inline bool NormalizeInit(const void* input, OrbisVrTrackerInitParam& out) {
    if (!input)
        return false;
    u32 size{};
    std::memcpy(&size, input, sizeof(size));
    if (size == sizeof(out)) {
        std::memcpy(&out, input, sizeof(out));
        return true;
    }
    if (size != 0x90)
        return false;
    out = {};
    const auto* bytes = static_cast<const u8*>(input);
    std::memcpy(&out, bytes, 0x20);
    std::memcpy(&out.calibration_settings, bytes + 0x20, 0x20);
    std::memcpy(&out.direct_memory_onion, bytes + 0x40, 0x10);
    std::memcpy(&out.direct_memory_garlic, bytes + 0x50, 0x10);
    std::memcpy(&out.work_memory, bytes + 0x60, 0x10);
    std::memcpy(&out.gpu_pipe_id, bytes + 0x70, 0x08);
    out.size = sizeof(out);
    return true;
}
static_assert(sizeof(OrbisVrTrackerInitParam) == 0x80);
static_assert(offsetof(OrbisVrTrackerInitParam, calibration_settings) == 0x28);
static_assert(offsetof(OrbisVrTrackerInitParam, direct_memory_onion) == 0x48);
} // namespace Libraries::VrTracker
