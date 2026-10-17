// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <algorithm>
#include <cstring>
#include <span>

#include "common/types.h"

namespace AmdGpu {

inline u32 Pm4Type0NumWords(u32 raw) {
    return ((raw >> 16) & 0x3fffu) + 1;
}

inline u32 Pm4Type0PacketDwords(u32 raw) {
    return Pm4Type0NumWords(raw) + 1;
}

inline u32 Pm4Type0Base(u32 raw) {
    return raw & 0xffffu;
}

inline u32 Pm4Type0HeaderRaw(u32 base, u32 num_words) {
    const u32 count = num_words == 0 ? 0 : num_words - 1;
    return (count << 16) | (base & 0xffffu);
}

struct Pm4Type0Consume {
    bool consumed = false;
    u32 packet_dwords = 0;
    u32 words_written = 0;
};

// Sequential GCN type-0 register writes. Must never abort: an UNREACHABLE here
// is Android exit 133 (SIGTRAP) and can take down the Adreno compositor.
inline Pm4Type0Consume ConsumePm4Type0(std::span<const u32> dcb, std::span<u32> regs) {
    if (dcb.empty() || (dcb[0] >> 30) != 0) {
        return {};
    }
    const u32 nwords = Pm4Type0NumWords(dcb[0]);
    const u32 packet = nwords + 1;
    if (packet > dcb.size()) {
        return {false, packet, 0};
    }
    const u32 base = Pm4Type0Base(dcb[0]);
    u32 written = 0;
    if (base < regs.size()) {
        written = std::min(nwords, static_cast<u32>(regs.size() - base));
        std::memcpy(regs.data() + base, dcb.data() + 1, written * sizeof(u32));
    }
    return {true, packet, written};
}

} // namespace AmdGpu
