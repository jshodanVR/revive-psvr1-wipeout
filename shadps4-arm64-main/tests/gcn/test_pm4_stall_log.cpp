// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <limits>

#include <gtest/gtest.h>
#include "video_core/amdgpu/stall_log.h"

TEST(Pm4StallLogTest, ShouldLogStallIteration) {
    EXPECT_FALSE(AmdGpu::ShouldLogStallIteration(0));
    EXPECT_FALSE(AmdGpu::ShouldLogStallIteration(10));
    EXPECT_FALSE(AmdGpu::ShouldLogStallIteration(99'999));
    EXPECT_TRUE(AmdGpu::ShouldLogStallIteration(100'000));
    EXPECT_FALSE(AmdGpu::ShouldLogStallIteration(100'001));
    EXPECT_TRUE(AmdGpu::ShouldLogStallIteration(200'000));
}

TEST(Pm4StallLogTest, GpuTaskPresentFenceIsNonblockingPoll) {
    EXPECT_EQ(AmdGpu::PresentFenceTimeoutNs(/*gpu_thread=*/true, /*in_gfx_task=*/true), 0u);
}

TEST(Pm4StallLogTest, IdleGpuThreadAndPresentThreadBlockOnFence) {
    EXPECT_EQ(AmdGpu::PresentFenceTimeoutNs(true, false), std::numeric_limits<u64>::max());
    EXPECT_EQ(AmdGpu::PresentFenceTimeoutNs(false, true), std::numeric_limits<u64>::max());
    EXPECT_EQ(AmdGpu::PresentFenceTimeoutNs(false, false), std::numeric_limits<u64>::max());
}
