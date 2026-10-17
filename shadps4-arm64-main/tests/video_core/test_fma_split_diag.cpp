// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <gtest/gtest.h>

#include "shader_recompiler/backend/spirv/fma_split_diag.h"

namespace {

using Shader::Backend::SPIRV::ScanSpirvFmaOps;
using Shader::Backend::SPIRV::ShouldSplitFmaNoContraction;
using Shader::Backend::SPIRV::SplitFmaSurvivedBackend;

TEST(FmaSplitDiag, SplitsKnownSkinnedVertexHashes) {
    EXPECT_TRUE(ShouldSplitFmaNoContraction(0x032fd69c, Shader::Stage::Vertex));
    EXPECT_TRUE(ShouldSplitFmaNoContraction(0x27904a0c, Shader::Stage::Vertex));
}

TEST(FmaSplitDiag, SkipsOtherStagesAndHashes) {
    EXPECT_FALSE(ShouldSplitFmaNoContraction(0x032fd69c, Shader::Stage::Fragment));
    EXPECT_FALSE(ShouldSplitFmaNoContraction(0x032fd69c, Shader::Stage::Compute));
    EXPECT_FALSE(ShouldSplitFmaNoContraction(0xbbffe457, Shader::Stage::Vertex));
    EXPECT_FALSE(ShouldSplitFmaNoContraction(0, Shader::Stage::Vertex));
}

TEST(FmaSplitDiag, ScanFindsSeparateMulAddAndNoFma) {
    constexpr u32 kSpvMagic = 0x07230203;
    constexpr u32 kOpFAdd = 129;
    constexpr u32 kOpFMul = 133;
    constexpr u32 kOpDecorate = 71;
    constexpr u32 kOpExecutionMode = 16;
    constexpr u32 kDecorationNoContraction = 42;
    constexpr u32 kExecutionModeContractionOff = 31;
    const auto inst = [](u32 word_count, u32 opcode) { return (word_count << 16) | opcode; };

    const u32 words[] = {
        kSpvMagic, 0x00010000, 0, 16, 0,
        inst(5, kOpFMul), 1, 2, 3, 4,
        inst(3, kOpDecorate), 2, kDecorationNoContraction,
        inst(5, kOpFAdd), 1, 5, 2, 6,
        inst(3, kOpDecorate), 5, kDecorationNoContraction,
        inst(3, kOpExecutionMode), 10, kExecutionModeContractionOff,
    };

    const auto scan = ScanSpirvFmaOps(words);
    EXPECT_EQ(scan.op_fmul, 1u);
    EXPECT_EQ(scan.op_fadd, 1u);
    EXPECT_EQ(scan.glsl_fma, 0u);
    EXPECT_EQ(scan.op_fma_khr, 0u);
    EXPECT_EQ(scan.no_contraction, 2u);
    EXPECT_EQ(scan.contraction_off, 1u);
    EXPECT_TRUE(SplitFmaSurvivedBackend(scan));
}

TEST(FmaSplitDiag, ScanDetectsGlslFmaAndFmaKhr) {
    constexpr u32 kSpvMagic = 0x07230203;
    constexpr u32 kOpExtInst = 12;
    constexpr u32 kOpFmaKHR = 4427;
    constexpr u32 kGLSLstd450Fma = 50;
    const auto inst = [](u32 word_count, u32 opcode) { return (word_count << 16) | opcode; };

    const u32 words[] = {
        kSpvMagic, 0x00010000, 0, 16, 0,
        inst(8, kOpExtInst), 1, 2, 3, kGLSLstd450Fma, 4, 5, 6,
        inst(6, kOpFmaKHR), 1, 7, 8, 9, 10,
    };

    const auto scan = ScanSpirvFmaOps(words);
    EXPECT_EQ(scan.glsl_fma, 1u);
    EXPECT_EQ(scan.op_fma_khr, 1u);
    EXPECT_FALSE(SplitFmaSurvivedBackend(scan));
}

} // namespace
