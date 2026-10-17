// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <span>

#include "common/types.h"
#include "shader_recompiler/runtime_info.h"

namespace Shader::Backend::SPIRV {

inline constexpr u64 kDsrSkinnedVs032fd69c = 0x032fd69c;
inline constexpr u64 kDsrSkinnedVs27904a0c = 0x27904a0c;

inline constexpr u32 kSpvOpExtInst = 12;
inline constexpr u32 kSpvOpExecutionMode = 16;
inline constexpr u32 kSpvOpFAdd = 129;
inline constexpr u32 kSpvOpFMul = 133;
inline constexpr u32 kSpvOpDecorate = 71;
inline constexpr u32 kSpvOpFmaKHR = 4427;
inline constexpr u32 kSpvDecorationNoContraction = 42;
inline constexpr u32 kSpvExecutionModeContractionOff = 31;
inline constexpr u32 kGLSLstd450Fma = 50;

[[nodiscard]] inline bool ShouldSplitFmaNoContraction(u64 pgm_hash, Stage stage) {
    if (stage != Stage::Vertex) {
        return false;
    }
    return pgm_hash == kDsrSkinnedVs032fd69c || pgm_hash == kDsrSkinnedVs27904a0c;
}

struct SpirvFmaScan {
    u32 op_fmul{};
    u32 op_fadd{};
    u32 glsl_fma{};
    u32 op_fma_khr{};
    u32 no_contraction{};
    u32 contraction_off{};
};

[[nodiscard]] inline SpirvFmaScan ScanSpirvFmaOps(std::span<const u32> words) {
    SpirvFmaScan scan{};
    if (words.size() < 5) {
        return scan;
    }
    size_t i = 5;
    while (i < words.size()) {
        const u32 first = words[i];
        const u32 word_count = first >> 16;
        const u32 opcode = first & 0xFFFFu;
        if (word_count == 0 || i + word_count > words.size()) {
            break;
        }
        switch (opcode) {
        case kSpvOpFMul:
            ++scan.op_fmul;
            break;
        case kSpvOpFAdd:
            ++scan.op_fadd;
            break;
        case kSpvOpFmaKHR:
            ++scan.op_fma_khr;
            break;
        case kSpvOpExtInst:
            if (word_count >= 5 && words[i + 4] == kGLSLstd450Fma) {
                ++scan.glsl_fma;
            }
            break;
        case kSpvOpDecorate:
            if (word_count >= 3 && words[i + 2] == kSpvDecorationNoContraction) {
                ++scan.no_contraction;
            }
            break;
        case kSpvOpExecutionMode:
            if (word_count >= 3 && words[i + 2] == kSpvExecutionModeContractionOff) {
                ++scan.contraction_off;
            }
            break;
        default:
            break;
        }
        i += word_count;
    }
    return scan;
}

[[nodiscard]] inline bool SplitFmaSurvivedBackend(const SpirvFmaScan& scan) {
    return scan.glsl_fma == 0 && scan.op_fma_khr == 0 && scan.op_fmul > 0 && scan.op_fadd > 0 &&
           scan.no_contraction > 0;
}

} // namespace Shader::Backend::SPIRV
