// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cmath>
#include <optional>
#include <nlohmann/json.hpp>

#include "core/vr/vr_runtime.h"

namespace Core::Vr {

inline std::optional<Fov> CachedHeadsetFov(const nlohmann::json& json,
                                           const HeadsetIdentity& identity) {
    if (identity.runtime.empty() || identity.system.empty() || !json.is_object() ||
        !json.contains("runtime") || json["runtime"] != identity.runtime ||
        !json.contains("system") || json["system"] != identity.system ||
        !json.contains("vendor_id") || json["vendor_id"] != identity.vendor_id ||
        !json.contains("fov_tan")) {
        return std::nullopt;
    }
    const auto& tangents = json["fov_tan"];
    if (!tangents.is_array() || tangents.size() != 4) {
        return std::nullopt;
    }
    for (const auto& tangent : tangents) {
        if (!tangent.is_number()) {
            return std::nullopt;
        }
        const float value = tangent.get<float>();
        if (!std::isfinite(value) || value < 0.1f || value > 10.0f) {
            return std::nullopt;
        }
    }
    return Fov{tangents[0].get<float>(), tangents[1].get<float>(), tangents[2].get<float>(),
               tangents[3].get<float>()};
}

}
