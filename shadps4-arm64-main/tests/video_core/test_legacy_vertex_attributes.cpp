// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <gtest/gtest.h>

#include "video_core/renderer_vulkan/legacy_vertex_attributes.h"

namespace {

using Vulkan::ShouldDisableLegacyVertexAttributes;
using Vulkan::ShouldForceBitPreservingBuffer0Loads;
using Vulkan::ShouldLoadFloatBufferAsU32;

TEST(LegacyVertexAttributes, DisablesOnAdreno830Unknown) {
    EXPECT_TRUE(ShouldDisableLegacyVertexAttributes("Adreno (TM) 830 (unknown)"));
}

TEST(LegacyVertexAttributes, DisablesOnAdreno840) {
    EXPECT_TRUE(ShouldDisableLegacyVertexAttributes("Adreno (TM) 840"));
}

TEST(LegacyVertexAttributes, KeepsEnabledOnAdreno750) {
    EXPECT_FALSE(ShouldDisableLegacyVertexAttributes("Turnip Adreno (TM) 750"));
}

TEST(LegacyVertexAttributes, KeepsEnabledOnDesktopAndEmpty) {
    EXPECT_FALSE(ShouldDisableLegacyVertexAttributes("NVIDIA GeForce RTX 4090"));
    EXPECT_FALSE(ShouldDisableLegacyVertexAttributes(""));
}

TEST(LegacyVertexAttributes, DoesNotMatchEmbedded830) {
    EXPECT_FALSE(ShouldDisableLegacyVertexAttributes("Adreno (TM) 1830"));
}

TEST(BitPreservingBuffer0Loads, ForcesOnAdreno830Unknown) {
    EXPECT_TRUE(ShouldForceBitPreservingBuffer0Loads("Adreno (TM) 830 (unknown)"));
}

TEST(BitPreservingBuffer0Loads, ForcesOnAdreno840) {
    EXPECT_TRUE(ShouldForceBitPreservingBuffer0Loads("Adreno (TM) 840"));
}

TEST(BitPreservingBuffer0Loads, SkipsAdreno750AndDesktop) {
    EXPECT_FALSE(ShouldForceBitPreservingBuffer0Loads("Turnip Adreno (TM) 750"));
    EXPECT_FALSE(ShouldForceBitPreservingBuffer0Loads("NVIDIA GeForce RTX 4090"));
    EXPECT_FALSE(ShouldForceBitPreservingBuffer0Loads(""));
}

TEST(BitPreservingBuffer0Loads, OnlyHandleZeroOnAdreno830) {
    EXPECT_TRUE(ShouldLoadFloatBufferAsU32("Adreno (TM) 830 (unknown)", 0));
    EXPECT_FALSE(ShouldLoadFloatBufferAsU32("Adreno (TM) 830 (unknown)", 1));
    EXPECT_FALSE(ShouldLoadFloatBufferAsU32("Turnip Adreno (TM) 750", 0));
}

} // namespace
