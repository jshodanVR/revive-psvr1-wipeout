// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <bit>
#include <cmath>
#include <span>

#include "common/types.h"
#include "shader_recompiler/runtime_info.h"

namespace Vulkan {

/// DSR skinned character VS hashes that explode on Adreno 830 when walking.
inline constexpr u64 kDsrSkinnedVs032fd69c = 0x032fd69c;
inline constexpr u64 kDsrSkinnedVs27904a0c = 0x27904a0c;

[[nodiscard]] inline bool ShouldIsolateReadConstSnapshot(u64 pgm_hash, Shader::Stage stage) {
    if (stage != Shader::Stage::Vertex) {
        return false;
    }
    return pgm_hash == kDsrSkinnedVs032fd69c || pgm_hash == kDsrSkinnedVs27904a0c;
}

[[nodiscard]] inline bool ShouldIsolateReadConstBuffer([[maybe_unused]] u64 pgm_hash,
                                                       [[maybe_unused]] Shader::Stage stage,
                                                       [[maybe_unused]] bool used_as_readconst,
                                                       [[maybe_unused]] bool is_written) {
    // Re-enabled for the recip-refine verification round (2026-08-16): the
    // snapshot bytes now also export the runtime-divisor dwords that feed the
    // rcp-based integer-division emulation addressed in
    // docs/dsr-adreno830-vertex-explosion.md §6. Costs ~40 -> ~19 FPS while
    // enabled; disable again for play builds.
    return ShouldIsolateReadConstSnapshot(pgm_hash, stage) && used_as_readconst && !is_written;
}

/// Float32 sanity of the ReadConst guest window. NaN/Inf while the mesh explodes
/// means garbage values entered the skinning math from the guest side; clean
/// floats under an exploding mesh point at GPU-side execution instead.
struct ReadConstFloatStats {
    u32 nan_count{};
    u32 inf_count{};
    u32 denorm_count{};
    u32 huge_count{}; // finite but |x| > 1e4
    float max_abs{};
};

inline constexpr float kReadConstHugeThreshold = 1.0e4f;

[[nodiscard]] inline ReadConstFloatStats ClassifyReadConstFloats(std::span<const u32> dwords) {
    ReadConstFloatStats stats{};
    for (const u32 raw : dwords) {
        const float f = std::bit_cast<float>(raw);
        if (std::isnan(f)) {
            ++stats.nan_count;
            continue;
        }
        if (std::isinf(f)) {
            ++stats.inf_count;
            continue;
        }
        if (std::fpclassify(f) == FP_SUBNORMAL) {
            ++stats.denorm_count;
            continue;
        }
        const float abs_f = std::fabs(f);
        if (abs_f > stats.max_abs) {
            stats.max_abs = abs_f;
        }
        if (abs_f > kReadConstHugeThreshold) {
            ++stats.huge_count;
        }
    }
    return stats;
}

} // namespace Vulkan
