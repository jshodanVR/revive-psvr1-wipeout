// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cmath>
#include "core/vr/vr_runtime.h"

namespace Core::Vr {
// The rear hand anchors one weapon. The support hand only steers it while
// gripping, in front of the rear hand and at a plausible two-handed distance.
inline DeviceState ComposeAimPose(const DeviceState& rear, const DeviceState& support,
                                  bool support_gripped) {
    auto result = rear;
    const auto finite = [](const Pose& p) {
        return std::isfinite(p.position.x) && std::isfinite(p.position.y) &&
               std::isfinite(p.position.z) && std::isfinite(p.orientation.x) &&
               std::isfinite(p.orientation.y) && std::isfinite(p.orientation.z) &&
               std::isfinite(p.orientation.w);
    };
    result.tracked = rear.tracked && finite(rear.pose);
    if (!result.tracked) return {};
    result.pose.orientation = Normalize(rear.pose.orientation);
    if (!support_gripped || !support.tracked || !finite(support.pose)) return result;
    Vec3 direction{support.pose.position.x - rear.pose.position.x,
                   support.pose.position.y - rear.pose.position.y,
                   support.pose.position.z - rear.pose.position.z};
    const float length = std::sqrt(direction.x * direction.x + direction.y * direction.y +
                                   direction.z * direction.z);
    if (length < 0.12f || length > 1.5f) return result;
    direction = {direction.x / length, direction.y / length, direction.z / length};
    const auto forward = Rotate(result.pose.orientation, {0, 0, -1});
    const float dot = forward.x * direction.x + forward.y * direction.y + forward.z * direction.z;
    // Crossing hands must not instantly turn the weapon backwards.
    if (dot < -0.25f) return result;
    const Quat delta{forward.y * direction.z - forward.z * direction.y,
                     forward.z * direction.x - forward.x * direction.z,
                     forward.x * direction.y - forward.y * direction.x, 1.0f + dot};
    result.pose.orientation = Normalize(Multiply(Normalize(delta), result.pose.orientation));
    return result;
}
} // namespace Core::Vr
