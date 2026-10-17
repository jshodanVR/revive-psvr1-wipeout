// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <span>

#include "common/types.h"
#include "video_core/amdgpu/pm4_type0.h"

namespace AmdGpu {

inline constexpr u32 kPm4Type1PacketDwords = 3;

inline u32 Pm4Type1RegA(u32 raw) {
    return raw & 0xffffu;
}

inline u32 Pm4Type1RegB(u32 raw) {
    return (raw >> 16) & 0x3fffu;
}

inline u32 Pm4Type1HeaderRaw(u32 reg_a, u32 reg_b) {
    return (1u << 30) | ((reg_b & 0x3fffu) << 16) | (reg_a & 0xffffu);
}

// Two non-consecutive GCN type-1 register writes. Must never abort: an
// UNREACHABLE here is Android exit 133 (SIGTRAP) and can take down the
// Adreno compositor. Driveclub CUSA00093 hits this on 3D entry after attract.
inline Pm4Type0Consume ConsumePm4Type1(std::span<const u32> dcb, std::span<u32> regs) {
    if (dcb.empty() || (dcb[0] >> 30) != 1) {
        return {};
    }
    if (dcb.size() < kPm4Type1PacketDwords) {
        return {false, kPm4Type1PacketDwords, 0};
    }
    const u32 reg_a = Pm4Type1RegA(dcb[0]);
    const u32 reg_b = Pm4Type1RegB(dcb[0]);
    u32 written = 0;
    if (reg_a < regs.size()) {
        regs[reg_a] = dcb[1];
        ++written;
    }
    if (reg_b < regs.size()) {
        regs[reg_b] = dcb[2];
        ++written;
    }
    return {true, kPm4Type1PacketDwords, written};
}

} // namespace AmdGpu
