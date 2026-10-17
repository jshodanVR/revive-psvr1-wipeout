// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#version 450

layout (location = 0) in vec2 uv;
layout (location = 0) out vec4 color;

layout (binding = 0) uniform sampler2D images[2];
#define texSampler images[0]

layout (push_constant) uniform settings {
    float gamma;
    bool hdr;
    // Above 0, the picture is sharpened by so much (up to 1): for one that is shown larger
    // than it was drawn, which blurs it.
    float sharpen;
    // The target encodes what it is given for display by itself (an sRGB image): it has to be
    // given linear light, or the picture would be encoded twice.
    bool linear_out;
    vec2 uv_scale;
    vec2 uv_offset;
    uint diagnostic_eye;
    bool overlay_enabled;
    vec2 overlay_scale;
    vec2 overlay_offset;
} pp;

const float cutoff = 0.0031308, a = 1.055, b = 0.055, d = 12.92;
vec3 gamma(vec3 rgb) {
    return mix(
        a * pow(rgb, vec3(1.0 / (2.4 + 1.0 - pp.gamma))) - b,
        d * rgb / pp.gamma,
        lessThan(rgb, vec3(cutoff))
    );
}

vec4 composited(vec2 at) {
    vec4 base = textureLod(texSampler, at, 0.0);
    if (pp.overlay_enabled) {
        vec2 eye_uv = (at - pp.uv_offset) / pp.uv_scale;
        vec2 top_uv = eye_uv * pp.overlay_scale + pp.overlay_offset;
        if (all(greaterThanEqual(top_uv, vec2(0))) && all(lessThanEqual(top_uv, vec2(1)))) {
            vec4 top = textureLod(images[1], top_uv, 0.0);
            base.rgb = top.rgb + base.rgb * (1.0 - top.a);
        }
    }
    return base;
}

// What goes to the display for the picture at a place.
vec3 shown(vec2 at) {
    vec2 half_texel = 0.5 / vec2(textureSize(texSampler, 0));
    at = clamp(at, pp.uv_offset + half_texel, pp.uv_offset + pp.uv_scale - half_texel);
    vec3 rgb = composited(at).rgb;
    return pp.hdr ? rgb : gamma(rgb);
}

void main() {
    if (pp.diagnostic_eye != 0) {
        // Diagnostic replaces the game with an unambiguous per-destination-eye label.
        // It intentionally tests routing, not binocular fusion or scene depth.
        vec2 p = (uv - vec2(0.36, 0.32)) / vec2(0.28, 0.36);
        bool inside = all(greaterThanEqual(p, vec2(0))) && all(lessThanEqual(p, vec2(1)));
        bool ink = p.x < 0.20;
        if (pp.diagnostic_eye == 1) {
            ink = ink || p.y > 0.82; // L
        } else {
            ink = ink || p.y < 0.16 || (p.y > 0.40 && p.y < 0.56) ||
                  (p.x > 0.80 && p.y < 0.56) ||
                  (p.y > 0.5 && abs(p.x - (0.2 + (p.y - 0.5) * 1.4)) < 0.13); // R
        }
        vec3 background = pp.diagnostic_eye == 1 ? vec3(0.18, 0.015, 0.015) : vec3(0.015, 0.05, 0.18);
        color = vec4(inside && ink ? vec3(0.65) : background, 1.0);
        return;
    }
    vec2 at = pp.uv_offset + uv * pp.uv_scale;
    vec2 half_texel = 0.5 / vec2(textureSize(texSampler, 0));
    at = clamp(at, pp.uv_offset + half_texel, pp.uv_offset + pp.uv_scale - half_texel);
    vec4 color_linear = composited(at);
    vec3 here = pp.hdr ? color_linear.rgb : gamma(color_linear.rgb);
    if (pp.sharpen > 0.0) {
        // Contrast adaptive sharpening: a pixel is pushed away from the four next to it, the
        // more the less they differ already, and never beyond black or white.
        vec2 texel = 1.0 / vec2(textureSize(texSampler, 0));
        vec3 above = shown(at - vec2(0.0, texel.y));
        vec3 below = shown(at + vec2(0.0, texel.y));
        vec3 left = shown(at - vec2(texel.x, 0.0));
        vec3 right = shown(at + vec2(texel.x, 0.0));
        vec3 darkest = min(min(min(above, below), min(left, right)), here);
        vec3 brightest = max(max(max(above, below), max(left, right)), here);
        vec3 room = sqrt(clamp(min(darkest, 1.0 - brightest) / max(brightest, vec3(1e-5)),
                               0.0, 1.0));
        vec3 weight = room * (-1.0 / mix(8.0, 5.0, clamp(pp.sharpen, 0.0, 1.0)));
        here = clamp(((above + below + left + right) * weight + here) / (1.0 + 4.0 * weight),
                     0.0, 1.0);
    }
    if (pp.linear_out && !pp.hdr) {
        // Back to linear light, by the curve the target encodes with.
        here = mix(pow((here + 0.055) / 1.055, vec3(2.4)), here / 12.92,
                   lessThanEqual(here, vec3(0.04045)));
    }
    color = vec4(here, color_linear.a);
}
