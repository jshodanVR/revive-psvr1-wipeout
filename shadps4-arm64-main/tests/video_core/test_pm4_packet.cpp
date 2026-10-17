// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <array>

#include <gtest/gtest.h>

#include "video_core/amdgpu/pm4_packet.h"
#include "video_core/amdgpu/pm4_type0.h"
#include "video_core/amdgpu/pm4_type1.h"

namespace {

using AmdGpu::kPm4Type1PacketDwords;
using AmdGpu::Pm4IndirectBufferUsable;
using AmdGpu::Pm4PacketDwords;
using AmdGpu::Pm4PacketFits;
using AmdGpu::Pm4Type0HeaderRaw;
using AmdGpu::Pm4Type1HeaderRaw;

// Driveclub CUSA00093 3D entry on OnePlus 13:
// after type-0/type-1 setup, NextPacket overflow then
// "Unhandled access violation ... Read from address 0x0".
// Truncated type-3 bodies and null IB/predicate pointers must not be followed.

TEST(Pm4Packet, Type0AndType1AndType2Sizes) {
    EXPECT_EQ(Pm4PacketDwords(Pm4Type0HeaderRaw(1, 1)), 2u);
    EXPECT_EQ(Pm4PacketDwords(Pm4Type1HeaderRaw(4, 10357)), kPm4Type1PacketDwords);
    EXPECT_EQ(Pm4PacketDwords(0x80000000u), 1u);
}

TEST(Pm4Packet, Type3SizeIsCountPlusTwo) {
    // type=3, count=3 → 3 body words + header = 5 dwords (Driveclub leftover 3)
    const u32 header = (3u << 30) | (3u << 16);
    EXPECT_EQ(Pm4PacketDwords(header), 5u);
}

TEST(Pm4Packet, TruncatedType3DoesNotFit) {
    const u32 header = (3u << 30) | (3u << 16);
    const std::array<u32, 3> dcb{header, 0, 0};
    EXPECT_FALSE(Pm4PacketFits(dcb));
}

TEST(Pm4Packet, FullType1Fits) {
    const std::array<u32, 3> dcb{Pm4Type1HeaderRaw(4, 10357), 1, 2};
    EXPECT_TRUE(Pm4PacketFits(dcb));
}

TEST(Pm4Packet, NullIndirectBufferIsNotUsable) {
    EXPECT_FALSE(Pm4IndirectBufferUsable(nullptr, 16));
    EXPECT_FALSE(Pm4IndirectBufferUsable(reinterpret_cast<const void*>(0x1000), 0));
    EXPECT_TRUE(Pm4IndirectBufferUsable(reinterpret_cast<const void*>(0x1000), 16));
}

TEST(Pm4Packet, NullGuestPredicateIsNotUsable) {
    EXPECT_FALSE(AmdGpu::Pm4GuestPointerUsable(nullptr));
    EXPECT_TRUE(AmdGpu::Pm4GuestPointerUsable(reinterpret_cast<const void*>(0x2000)));
}

// Driveclub 3D entry: ObtainBuffer(0) → StreamBuffer::Copy(0) → memcpy from
// nullptr → SignalHandler "Read from address 0x0" / exit 133.
TEST(Pm4Packet, ZeroGuestBufferIsNotCopyable) {
    EXPECT_FALSE(AmdGpu::GpuBufferCopySourceUsable(0, 64));
    EXPECT_FALSE(AmdGpu::GpuBufferCopySourceUsable(0x1000, 0));
    EXPECT_TRUE(AmdGpu::GpuBufferCopySourceUsable(0x1000, 64));
}

} // namespace
