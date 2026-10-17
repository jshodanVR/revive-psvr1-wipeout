// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "common/types.h"
#include "shader_recompiler/backend/spirv/fma_split_diag.h"
#include "shader_recompiler/runtime_info.h"

namespace Shader::Backend::SPIRV {

/// GCN `v_rcp_f32` guarantees <= 1 ulp. Vulkan allows `OpFDiv` up to 2.5 ulp,
/// and Turnip lowers `1.0 / x` to a coarser approximate reciprocal on some
/// Adreno generations (observed on the A830). DSR's skinned player shaders
/// divide the runtime vertex index by a per-frame divisor using the GCN
/// rcp-based integer-division emulation (rcp -> x 2^32 -> truncate -> 64-bit
/// magic multiply -> fixups), and the fixups only produce the exact quotient
/// while the reciprocal error stays inside the envelope the original GCN
/// compiler validated. Outside it the quotient is wrong, every vertex of the
/// walking mesh is fetched at quotient x 80 bytes off, and the mesh explodes
/// while idle (different divisor values) stays intact.
[[nodiscard]] inline bool ShouldRefineRecip32(u64 pgm_hash, Stage stage) {
    if (stage != Stage::Vertex) {
        return false;
    }
    return pgm_hash == kDsrSkinnedVs032fd69c || pgm_hash == kDsrSkinnedVs27904a0c;
}

} // namespace Shader::Backend::SPIRV
