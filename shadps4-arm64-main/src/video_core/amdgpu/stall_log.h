// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <limits>

#include "common/types.h"

namespace AmdGpu {

constexpr u64 STALL_LOG_INTERVAL = 100'000;

/// Returns true if the iteration count is a non-zero exact multiple of the log interval.
constexpr bool ShouldLogStallIteration(u64 iteration) {
    return iteration != 0 && (iteration % STALL_LOG_INTERVAL == 0);
}

/// GpuComm EOP flips call PrepareFrame while Process() is inside task.resume().
/// An infinite present-fence wait there stalls the PM4 processor, so Flip never
/// queues and VO WaitVoLabel never wakes. Poll (timeout 0) in that case.
constexpr u64 PresentFenceTimeoutNs(bool gpu_thread, bool in_gfx_task) {
    if (gpu_thread && in_gfx_task) {
        return 0;
    }
    return std::numeric_limits<u64>::max();
}

} // namespace AmdGpu
