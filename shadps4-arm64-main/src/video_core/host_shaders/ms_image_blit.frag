// SPDX-FileCopyrightText: Copyright 2025 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#version 450 core
#extension GL_EXT_samplerless_texture_functions : require

#if defined(SRC_MSAA)
layout (binding = 0, set = 0) uniform texture2DMS in_tex;
#else
layout (binding = 0, set = 0) uniform texture2D in_tex;
#endif

layout (location = 0) in vec2 uv;
layout (location = 0) out vec4 out_color;

void main()
{
#if defined(SRC_MSAA)
    // The destination may have more samples than the source.
    out_color = texelFetch(in_tex, ivec2(gl_FragCoord.xy),
                           min(gl_SampleID, textureSamples(in_tex) - 1));
#else
    out_color = texelFetch(in_tex, ivec2(gl_FragCoord.xy), 0);
#endif
}
