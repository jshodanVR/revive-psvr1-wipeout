// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <gtest/gtest.h>

#include <bit>
#include <vector>

#include "video_core/renderer_vulkan/readconst_snapshot_diag.h"

namespace {

using Vulkan::ClassifyReadConstFloats;
using Vulkan::ShouldIsolateReadConstBuffer;
using Vulkan::ShouldIsolateReadConstSnapshot;

TEST(ReadConstSnapshotDiag, IsolatesKnownSkinnedVertexHashes) {
    EXPECT_TRUE(ShouldIsolateReadConstSnapshot(0x032fd69c, Shader::Stage::Vertex));
    EXPECT_TRUE(ShouldIsolateReadConstSnapshot(0x27904a0c, Shader::Stage::Vertex));
}

TEST(ReadConstSnapshotDiag, SkipsOtherStagesAndHashes) {
    EXPECT_FALSE(ShouldIsolateReadConstSnapshot(0x032fd69c, Shader::Stage::Fragment));
    EXPECT_FALSE(ShouldIsolateReadConstSnapshot(0x032fd69c, Shader::Stage::Compute));
    EXPECT_FALSE(ShouldIsolateReadConstSnapshot(0xbbffe457, Shader::Stage::Vertex));
    EXPECT_FALSE(ShouldIsolateReadConstSnapshot(0, Shader::Stage::Vertex));
}

TEST(ReadConstSnapshotDiag, IsolationDisabledAfterTestB) {
    // The isolation diagnostic is disabled at the source after Test B: the
    // per-draw memcpy + scheduler.Finish() costs ~40 -> ~19 FPS in DSR.
    // Every request must be rejected regardless of hash/stage/flags.
    EXPECT_FALSE(ShouldIsolateReadConstBuffer(0x032fd69c, Shader::Stage::Vertex, true, false));
    EXPECT_FALSE(ShouldIsolateReadConstBuffer(0x27904a0c, Shader::Stage::Vertex, true, false));
    EXPECT_FALSE(ShouldIsolateReadConstBuffer(0x032fd69c, Shader::Stage::Vertex, false, false));
    EXPECT_FALSE(ShouldIsolateReadConstBuffer(0x032fd69c, Shader::Stage::Vertex, true, true));
    EXPECT_FALSE(ShouldIsolateReadConstBuffer(0xbbffe457, Shader::Stage::Vertex, true, false));
}

TEST(ReadConstSnapshotDiag, ClassifiesCleanBoneMatrixWindow) {
    const std::vector<u32> dwords{
        std::bit_cast<u32>(1.0f),   std::bit_cast<u32>(0.0f),  std::bit_cast<u32>(-0.5f),
        std::bit_cast<u32>(42.25f), std::bit_cast<u32>(-2.0f), std::bit_cast<u32>(0.03125f),
    };
    const auto stats = ClassifyReadConstFloats(dwords);
    EXPECT_EQ(stats.nan_count, 0u);
    EXPECT_EQ(stats.inf_count, 0u);
    EXPECT_EQ(stats.denorm_count, 0u);
    EXPECT_EQ(stats.huge_count, 0u);
    EXPECT_FLOAT_EQ(stats.max_abs, 42.25f);
}

TEST(ReadConstSnapshotDiag, DetectsNonFiniteAndHugeFloats) {
    const std::vector<u32> dwords{
        0x7fc00000, // quiet NaN
        0xff800000, // -Inf
        0x7f800000, // +Inf
        0x00000001, // subnormal
        std::bit_cast<u32>(-1.0e5f),
        std::bit_cast<u32>(1.0e4f), // exactly at threshold: not huge
    };
    const auto stats = ClassifyReadConstFloats(dwords);
    EXPECT_EQ(stats.nan_count, 1u);
    EXPECT_EQ(stats.inf_count, 2u);
    EXPECT_EQ(stats.denorm_count, 1u);
    EXPECT_EQ(stats.huge_count, 1u);
    EXPECT_FLOAT_EQ(stats.max_abs, 1.0e5f);
}

TEST(ReadConstSnapshotDiag, EmptyWindowIsClean) {
    const auto stats = ClassifyReadConstFloats({});
    EXPECT_EQ(stats.nan_count, 0u);
    EXPECT_EQ(stats.inf_count, 0u);
    EXPECT_EQ(stats.denorm_count, 0u);
    EXPECT_EQ(stats.huge_count, 0u);
    EXPECT_FLOAT_EQ(stats.max_abs, 0.0f);
}

} // namespace
