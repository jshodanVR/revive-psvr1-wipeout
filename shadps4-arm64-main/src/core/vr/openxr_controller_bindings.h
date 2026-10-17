// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <array>
#include <cmath>
#include <string>
#include <string_view>

namespace Core::Vr {
enum class ControllerAction {
    Move, Finger, FingerPress, Cross, Square, Circle, Triangle,
    L1, R1, L2, R2, Options, L3, Pose, Grip, Rumble,
    LeftGripClick, RightGripClick, MenuForce,
    LeftFacePad, RightFacePad, LeftFaceClick, RightFaceClick,
    LeftBumper, RightBumper, DpadUp, DpadDown, DpadLeft, DpadRight, TouchpadPress, Count
};
enum class ControllerProfile { Touch, Index, Motion, Vive, Frame };
inline constexpr std::array ControllerProfiles{
    ControllerProfile::Touch, ControllerProfile::Index,
    ControllerProfile::Motion, ControllerProfile::Vive, ControllerProfile::Frame};
inline constexpr const char* FrameControllerExtension = "XR_VALVE_frame_controller_interaction";
inline bool CanSuggestProfile(ControllerProfile profile, bool frame_enabled) {
    return profile != ControllerProfile::Frame || frame_enabled;
}
enum class RightStickMode { Touchpad, Analog, Dpad };
inline RightStickMode SelectRightStickMode(bool frame, bool view, bool aim, bool dpad) {
    if (frame) return view ? RightStickMode::Touchpad : RightStickMode::Analog;
    return dpad ? RightStickMode::Dpad : aim ? RightStickMode::Analog : RightStickMode::Touchpad;
}

inline constexpr std::string_view ProfilePath(ControllerProfile profile) {
    switch (profile) {
    case ControllerProfile::Frame: return "/interaction_profiles/valve/frame_controller_valve";
    case ControllerProfile::Touch: return "/interaction_profiles/oculus/touch_controller";
    case ControllerProfile::Index: return "/interaction_profiles/valve/index_controller";
    case ControllerProfile::Motion: return "/interaction_profiles/microsoft/motion_controller";
    case ControllerProfile::Vive: return "/interaction_profiles/htc/vive_controller";
    }
    return {};
}

// Binding paths are runtime interaction profiles, never headset model checks.
// Keep this table independent of the host so every path can be checked against xr.xml.
template <typename Emit>
void ForEachControllerBinding(ControllerProfile profile, int primary_hand, Emit emit) {
    using A = ControllerAction;
    const auto bind = [&](A action, int hand, std::string_view component) {
        emit(action, std::string(hand == 0 ? "/user/hand/left" : "/user/hand/right") +
                         std::string(component));
    };
    for (int hand = 0; hand < 2; ++hand) {
        bind(A::Grip, hand, "/input/grip/pose");
        bind(A::Rumble, hand, "/output/haptic");
        bind(hand == 0 ? A::L2 : A::R2, hand, "/input/trigger/value");
    }
    bind(A::Pose, primary_hand, "/input/aim/pose");
    const bool pads_only = profile == ControllerProfile::Vive;
    bind(A::Move, 0, pads_only ? "/input/trackpad" : "/input/thumbstick");
    bind(A::Finger, 1, pads_only ? "/input/trackpad" : "/input/thumbstick");
    if (!pads_only) {
        bind(A::L3, 0, "/input/thumbstick/click");
        bind(A::FingerPress, 1, "/input/thumbstick/click");
    }
    if (profile == ControllerProfile::Frame) {
        bind(A::Cross, 1, "/input/a/click");
        bind(A::Circle, 1, "/input/b/click");
        bind(A::Square, 1, "/input/x/click");
        bind(A::Triangle, 1, "/input/y/click");
        bind(A::DpadUp, 0, "/input/dpad_up/click");
        bind(A::DpadDown, 0, "/input/dpad_down/click");
        bind(A::DpadLeft, 0, "/input/dpad_left/click");
        bind(A::DpadRight, 0, "/input/dpad_right/click");
        bind(A::LeftBumper, 0, "/input/bumper/click");
        bind(A::RightBumper, 1, "/input/bumper/click");
        bind(A::L1, 0, "/input/squeeze/value");
        bind(A::R1, 1, "/input/squeeze/value");
        bind(A::Options, 1, "/input/menu/click");
        bind(A::TouchpadPress, 0, "/input/view/click");
    } else if (profile == ControllerProfile::Touch || profile == ControllerProfile::Index) {
        bind(A::Cross, 1, "/input/a/click");
        bind(A::Square, 1, "/input/b/click");
        bind(A::Circle, 0, profile == ControllerProfile::Touch ? "/input/x/click" : "/input/a/click");
        bind(A::Triangle, 0, profile == ControllerProfile::Touch ? "/input/y/click" : "/input/b/click");
        bind(A::L1, 0, "/input/squeeze/value");
        bind(A::R1, 1, "/input/squeeze/value");
        if (profile == ControllerProfile::Touch) bind(A::Options, 0, "/input/menu/click");
        // Index's system button belongs to the runtime. Press the left trackpad instead.
        else bind(A::MenuForce, 0, "/input/trackpad/force");
    } else {
        bind(A::LeftGripClick, 0, "/input/squeeze/click");
        bind(A::RightGripClick, 1, "/input/squeeze/click");
        bind(A::Options, 0, "/input/menu/click");
        for (int hand = 0; hand < 2; ++hand) {
            bind(hand == 0 ? A::LeftFacePad : A::RightFacePad, hand, "/input/trackpad");
            bind(hand == 0 ? A::LeftFaceClick : A::RightFaceClick, hand, "/input/trackpad/click");
        }
    }
}

enum class FacePadButton { None, Cross, Square, Circle, Triangle };
inline FacePadButton DecodeFacePad(float x, float y, bool clicked) {
    if (!clicked || !std::isfinite(x) || !std::isfinite(y)) return FacePadButton::None;
    if (std::hypot(x, y) < 0.35f) return FacePadButton::Cross;
    if (std::abs(x) > std::abs(y)) return x < 0 ? FacePadButton::Square : FacePadButton::Circle;
    return y > 0 ? FacePadButton::Triangle : FacePadButton::Cross;
}
} // namespace Core::Vr
