// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <span>

#include "common/types.h"
#include "video_core/amdgpu/pm4_type0.h"
#include "video_core/amdgpu/pm4_type1.h"

namespace AmdGpu {

// GCN packet size in dwords. Type-1 REG_B shares the type-3 count field,
// so size must come from the type bits, not type3.NumWords().
inline u32 Pm4PacketDwords(u32 raw) {
    switch (raw >> 30) {
    case 0:
        return Pm4Type0PacketDwords(raw);
    case 1:
        return kPm4Type1PacketDwords;
    case 2:
        return 1;
    case 3:
        return ((raw >> 16) & 0x3fffu) + 2;
    default:
        return 0;
    }
}

inline bool Pm4PacketFits(std::span<const u32> dcb) {
    if (dcb.empty()) {
        return false;
    }
    const u32 n = Pm4PacketDwords(dcb[0]);
    return n > 0 && n <= dcb.size();
}

inline bool Pm4IndirectBufferUsable(const void* addr, u32 ib_dwords) {
    return addr != nullptr && ib_dwords > 0;
}

inline bool Pm4GuestPointerUsable(const void* addr) {
    return addr != nullptr;
}

inline bool GpuBufferCopySourceUsable(u64 device_addr, u32 size) {
    return device_addr != 0 && size != 0;
}

} // namespace AmdGpu
