// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <array>
#include <span>

#include <gtest/gtest.h>

#include "video_core/amdgpu/pm4_type1.h"

namespace {

using AmdGpu::ConsumePm4Type1;
using AmdGpu::kPm4Type1PacketDwords;
using AmdGpu::Pm4Type1HeaderRaw;
using AmdGpu::Pm4Type1RegA;
using AmdGpu::Pm4Type1RegB;

// Driveclub CUSA00093 on OnePlus 13 (Adreno 830):
// liverpool.cpp UNREACHABLE "Wrong PM4 type 1" after attract skip / 3D entry
// -> SIGTRAP / Android exit 133.
TEST(Pm4Type1, DriveclubType1PacketIsConsumedNotRejected) {
    const u32 header = Pm4Type1HeaderRaw(4, 9);
    const std::array<u32, 3> dcb{header, 0x1111u, 0x2222u};
    std::array<u32, 16> regs{};
    const auto result = ConsumePm4Type1(dcb, regs);
    EXPECT_TRUE(result.consumed);
    EXPECT_EQ(result.packet_dwords, kPm4Type1PacketDwords);
    EXPECT_EQ(result.words_written, 2u);
    EXPECT_EQ(regs[4], 0x1111u);
    EXPECT_EQ(regs[9], 0x2222u);
}

TEST(Pm4Type1, WritesTwoNonSequentialRegisters) {
    const u32 header = Pm4Type1HeaderRaw(1, 5);
    const std::array<u32, 3> dcb{header, 42u, 99u};
    std::array<u32, 8> regs{};
    const auto result = ConsumePm4Type1(dcb, regs);
    EXPECT_TRUE(result.consumed);
    EXPECT_EQ(regs[1], 42u);
    EXPECT_EQ(regs[5], 99u);
    EXPECT_EQ(regs[0], 0u);
    EXPECT_EQ(regs[2], 0u);
    EXPECT_EQ(regs[4], 0u);
}

TEST(Pm4Type1, TruncatedPacketIsNotConsumed) {
    const u32 header = Pm4Type1HeaderRaw(0, 1);
    const std::array<u32, 1> dcb{header};
    std::array<u32, 8> regs{};
    const auto result = ConsumePm4Type1(dcb, regs);
    EXPECT_FALSE(result.consumed);
    EXPECT_EQ(result.packet_dwords, kPm4Type1PacketDwords);
    EXPECT_EQ(result.words_written, 0u);
    EXPECT_EQ(regs[0], 0u);
}

TEST(Pm4Type1, Type0AndType3HeadersAreNotConsumed) {
    std::array<u32, 8> regs{};
    const std::array<u32, 3> type0{0x00010000u, 1, 2};
    const auto r0 = ConsumePm4Type1(type0, regs);
    EXPECT_FALSE(r0.consumed);
    EXPECT_EQ(r0.packet_dwords, 0u);

    const std::array<u32, 2> type3{0xC0000000u, 0};
    const auto r3 = ConsumePm4Type1(type3, regs);
    EXPECT_FALSE(r3.consumed);
    EXPECT_EQ(r3.packet_dwords, 0u);
}

TEST(Pm4Type1, HeaderHelpersMatchGcnLayout) {
    const u32 header = Pm4Type1HeaderRaw(0x12, 0x34);
    EXPECT_EQ(header >> 30, 1u);
    EXPECT_EQ(Pm4Type1RegA(header), 0x12u);
    EXPECT_EQ(Pm4Type1RegB(header), 0x34u);
    EXPECT_EQ(kPm4Type1PacketDwords, 3u);
}

} // namespace
