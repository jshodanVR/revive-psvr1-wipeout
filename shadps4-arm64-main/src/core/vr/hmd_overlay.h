// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <array>
#include <cmath>
#include <optional>

namespace Core::Vr {
struct OverlayUvTransform {
    std::array<float, 2> scale;
    std::array<float, 2> offset;
};
// Eye UVs are scale_x, scale_y, offset_x, offset_y. Both layers share tan space.
inline std::optional<OverlayUvTransform> OverlayTransform(
    std::array<float, 4> base, const std::array<float, 4>& top, bool sbs, int eye) {
    if (eye < 0 || eye > 1) return std::nullopt;
    for (float v : base) if (!std::isfinite(v)) return std::nullopt;
    for (float v : top) if (!std::isfinite(v)) return std::nullopt;
    if (base[0] <= 0 || base[1] <= 0 || top[0] <= 0 || top[1] <= 0)
        return std::nullopt;
    if (sbs) { base[0] *= 2; base[2] = base[2] * 2 - eye; }
    OverlayUvTransform result{{top[0] / base[0], top[1] / base[1]}, {}};
    result.offset = {top[2] - base[2] * result.scale[0],
                     top[3] - base[3] * result.scale[1]};
    for (float v : result.scale) if (!std::isfinite(v)) return std::nullopt;
    for (float v : result.offset) if (!std::isfinite(v)) return std::nullopt;
    return result;
}
} // namespace Core::Vr
