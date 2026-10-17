// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <optional>
#include <openxr/openxr.h>

#include "core/vr/vr_runtime.h"

namespace Core::Vr {

struct ViewTangents {
    float left;
    float right;
    float up;
    float down;
};

inline std::optional<ViewTangents> ParallelViewTangents(const std::array<Vec3, 4>& corners) {
    if (std::ranges::any_of(corners, [](const Vec3& ray) {
            return !std::isfinite(ray.x) || !std::isfinite(ray.y) || !std::isfinite(ray.z) ||
                   ray.z >= -1e-5f;
        })) {
        return std::nullopt;
    }
    const auto horizontal =
        std::minmax({-corners[0].x / corners[0].z, -corners[1].x / corners[1].z,
                     -corners[2].x / corners[2].z, -corners[3].x / corners[3].z});
    const auto vertical = std::minmax({-corners[0].y / corners[0].z, -corners[1].y / corners[1].z,
                                       -corners[2].y / corners[2].z, -corners[3].y / corners[3].z});
    return ViewTangents{-horizontal.first, horizontal.second, vertical.second, -vertical.first};
}

inline std::optional<ViewTangents> ParallelViewTangents(const XrView& view) {
    const auto& q = view.pose.orientation;
    const float length_squared = q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w;
    if (!std::isfinite(length_squared) || length_squared < 1e-12f) {
        return std::nullopt;
    }
    const Quat orientation = Normalize({q.x, q.y, q.z, q.w});
    const float left = std::tan(view.fov.angleLeft);
    const float right = std::tan(view.fov.angleRight);
    const float up = std::tan(view.fov.angleUp);
    const float down = std::tan(view.fov.angleDown);
    return ParallelViewTangents({Rotate(orientation, Vec3{left, up, -1.0f}),
                                 Rotate(orientation, Vec3{right, up, -1.0f}),
                                 Rotate(orientation, Vec3{left, down, -1.0f}),
                                 Rotate(orientation, Vec3{right, down, -1.0f})});
}

inline std::optional<Fov> ParallelStereoFov(const XrView& left, const XrView& right) {
    const auto left_view = ParallelViewTangents(left);
    const auto right_view = ParallelViewTangents(right);
    if (!left_view || !right_view) {
        return std::nullopt;
    }
    const Fov seen{
        std::max(left_view->left, right_view->right),
        std::max(left_view->right, right_view->left),
        std::max(left_view->up, right_view->up),
        std::max(left_view->down, right_view->down),
    };
    for (const float tangent : {seen.tan_out, seen.tan_in, seen.tan_top, seen.tan_bottom}) {
        if (tangent <= 0.1f || tangent >= 10.0f || !std::isfinite(tangent)) {
            return std::nullopt;
        }
    }
    return seen;
}

}
