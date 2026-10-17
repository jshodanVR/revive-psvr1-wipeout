// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <array>
#include <span>
#include <vector>

#include <gtest/gtest.h>

#include "video_core/amdgpu/pm4_type0.h"

namespace {

using AmdGpu::ConsumePm4Type0;
using AmdGpu::Pm4Type0HeaderRaw;
using AmdGpu::Pm4Type0NumWords;
using AmdGpu::Pm4Type0PacketDwords;

// Driveclub CUSA00093 on OnePlus 13 (Adreno 830):
// liverpool.cpp UNREACHABLE "Unimplemented PM4 type 0, base reg: 0, size: 1185"
// -> SIGTRAP / Android exit 133 and can take the compositor down.
TEST(Pm4Type0, Driveclub1185WordPacketIsConsumedNotRejected) {
    constexpr u32 nwords = 1185;
    std::vector<u32> dcb(nwords + 1, 0);
    dcb[0] = Pm4Type0HeaderRaw(0, nwords);
    for (u32 i = 0; i < nwords; ++i) {
        dcb[i + 1] = 0x1000 + i;
    }
    std::array<u32, 2048> regs{};
    const auto result = ConsumePm4Type0(dcb, regs);
    EXPECT_TRUE(result.consumed);
    EXPECT_EQ(result.packet_dwords, nwords + 1);
    EXPECT_EQ(result.words_written, nwords);
    EXPECT_EQ(regs[0], 0x1000u);
    EXPECT_EQ(regs[1184], 0x1000u + 1184);
}

TEST(Pm4Type0, WritesSequentialRegistersFromBase) {
    const u32 header = Pm4Type0HeaderRaw(4, 3);
    const std::array<u32, 4> dcb{header, 11, 22, 33};
    std::array<u32, 16> regs{};
    const auto result = ConsumePm4Type0(dcb, regs);
    EXPECT_TRUE(result.consumed);
    EXPECT_EQ(result.packet_dwords, 4u);
    EXPECT_EQ(result.words_written, 3u);
    EXPECT_EQ(regs[4], 11u);
    EXPECT_EQ(regs[5], 22u);
    EXPECT_EQ(regs[6], 33u);
    EXPECT_EQ(regs[3], 0u);
}

TEST(Pm4Type0, TruncatedPacketIsNotConsumed) {
    const u32 header = Pm4Type0HeaderRaw(0, 1185);
    const std::array<u32, 2> dcb{header, 1};
    std::array<u32, 16> regs{};
    const auto result = ConsumePm4Type0(dcb, regs);
    EXPECT_FALSE(result.consumed);
    EXPECT_EQ(result.packet_dwords, 1186u);
    EXPECT_EQ(result.words_written, 0u);
    EXPECT_EQ(regs[0], 0u);
}

TEST(Pm4Type0, Type3HeaderIsNotConsumed) {
    // type=3, count=0 -> 0xC0000000
    const std::array<u32, 2> dcb{0xC0000000u, 0};
    std::array<u32, 8> regs{};
    const auto result = ConsumePm4Type0(dcb, regs);
    EXPECT_FALSE(result.consumed);
    EXPECT_EQ(result.packet_dwords, 0u);
}

TEST(Pm4Type0, HeaderHelpersMatchGcnLayout) {
    EXPECT_EQ(Pm4Type0NumWords(Pm4Type0HeaderRaw(0, 1185)), 1185u);
    EXPECT_EQ(Pm4Type0PacketDwords(Pm4Type0HeaderRaw(0, 1185)), 1186u);
    EXPECT_EQ(Pm4Type0HeaderRaw(0, 1185) >> 30, 0u);
}

} // namespace
