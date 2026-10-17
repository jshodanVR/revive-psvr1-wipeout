// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <vector>
#include "common/types.h"

namespace Shader {
struct FragmentRuntimeInfo;
struct Info;
}

namespace Shader::Backend::SPIRV {

enum class AuxShaderType : u32 {
    RectListTCS,
    QuadListTCS,
    PassthroughTES,
};

[[nodiscard]] std::vector<u32> EmitAuxilaryTessShader(AuxShaderType type,
                                                      const FragmentRuntimeInfo& fs_info,
                                                      s32 layer_location = -1);

/// Reserve an unused user varying for Layer across auxiliary tessellation stages.
[[nodiscard]] u32 AuxLayerLocation(const Info& info, bool clip_emulation = false);

} // namespace Shader::Backend::SPIRV
