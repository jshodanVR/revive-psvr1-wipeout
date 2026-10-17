// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <gtest/gtest.h>

#include "shader_recompiler/backend/spirv/recip_refine_fix.h"

namespace {

using Shader::Backend::SPIRV::ShouldRefineRecip32;

TEST(RecipRefineFix, RefinesKnownSkinnedVertexHashes) {
    EXPECT_TRUE(ShouldRefineRecip32(0x032fd69c, Shader::Stage::Vertex));
    EXPECT_TRUE(ShouldRefineRecip32(0x27904a0c, Shader::Stage::Vertex));
}

TEST(RecipRefineFix, SkipsOtherStagesAndHashes) {
    EXPECT_FALSE(ShouldRefineRecip32(0x032fd69c, Shader::Stage::Fragment));
    EXPECT_FALSE(ShouldRefineRecip32(0x032fd69c, Shader::Stage::Compute));
    EXPECT_FALSE(ShouldRefineRecip32(0xbbffe457, Shader::Stage::Vertex));
    EXPECT_FALSE(ShouldRefineRecip32(0, Shader::Stage::Vertex));
}

} // namespace
