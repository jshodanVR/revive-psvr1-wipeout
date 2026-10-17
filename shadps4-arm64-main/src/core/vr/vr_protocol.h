// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstdint>

// Messages between the emulator and the application that owns the real headset (the "VR host").
// They travel over a SOCK_SEQPACKET unix socket the host listens on; the emulator finds it through
// the SHADPS4_VR_SOCKET environment variable. One message per packet, native byte order, both
// ends run on the same machine. This header is shared with the host application and therefore
// depends on nothing else in the emulator.

namespace Core::Vr::Protocol {

inline constexpr std::uint32_t Magic = 0x31525641; // "AVR1"
inline constexpr std::uint32_t MaxBuffers = 4;

enum class MessageType : std::uint32_t {
    Buffers = 1,
    Frame = 2,
    Pose = 3,
    Optics = 4,
    PadPose = 5,
    PadFeedback = 6,
    Refresh = 7,
};

struct Header {
    std::uint32_t magic{Magic};
    MessageType type{};
};

/// Host to emulator, once: the images frames are delivered in. Each is a dma-buf holding
/// `height` rows of `stride` four-byte pixels, passed as ancillary file descriptors in order.
struct Buffers {
    Header header{.type = MessageType::Buffers};
    std::uint32_t count{};
    std::uint32_t width{};
    std::uint32_t height{};
    std::uint32_t stride{};
};

/// Emulator to host: buffer `buffer` now holds a finished frame, the left eye in its left half
/// and the right eye in its right half.
struct Frame {
    Header header{.type = MessageType::Frame};
    std::uint32_t buffer{};
    std::uint32_t frame_id{};
    /// Resolution the guest rendered each eye at, before scaling into the buffer.
    std::uint32_t eye_width{};
    std::uint32_t eye_height{};
    /// Head pose the frame was rendered for, in host space.
    float position[3]{};
    float orientation[4]{0.0f, 0.0f, 0.0f, 1.0f};
    /// Tangents of the half angles: towards the temple, the nose, up and down.
    float fov[4]{};
    /// Non-zero when the bytes of a pixel are blue, green, red, alpha instead of red first.
    std::uint32_t swap_red_blue{};
};

/// Host to emulator, every display refresh: where the head is expected to be when the next
/// frame is shown. Host space is metres, +X right, +Y up, -Z forward; its origin and what counts
/// as forward are the headset system's affair (the emulator keeps track of where in it the
/// player sits, see PadPose::RecenterSeat).
struct Pose {
    Header header{.type = MessageType::Pose};
    float position[3]{};
    float orientation[4]{0.0f, 0.0f, 0.0f, 1.0f};
    float linear_velocity[3]{};
    float angular_velocity[3]{};
};

/// Host to emulator, each time the host starts on the picture for the next refresh of its
/// display. The emulated headset then refreshes in step with the real one instead of by a clock
/// of its own, which would drift against it: a title that draws one frame for every two
/// refreshes has each frame shown for two refreshes exactly, not for one or three by turns.
struct Refresh {
    Header header{.type = MessageType::Refresh};
    /// Refreshes of the display per second.
    float rate{};
    /// When the host started on the picture, on the clock both ends share (CLOCK_MONOTONIC, in
    /// nanoseconds): the message itself may be picked up late. 0 if the host cannot say.
    std::uint64_t time{};
};

/// Host to emulator: properties of the wearer and the headset.
struct Optics {
    Header header{.type = MessageType::Optics};
    /// Distance between the eyes in metres.
    float ipd{};
};

/// Host to emulator: what the host can see of the controller the player holds. A gamepad has no
/// tracking of its own, but the headset may see the hands around it, or a tracked device may be
/// fixed to it. Whatever is not flagged valid the emulator works out for itself (from the
/// controller's motion sensors, or by holding it in front of the player).
struct PadPose {
    static constexpr std::uint32_t PositionValid = 1u << 0;
    /// `yaw` holds the heading: 0 when the controller points straight ahead (-Z), positive to
    /// the left. The sensors of a controller know its tilt but drift in heading.
    static constexpr std::uint32_t YawValid = 1u << 1;
    /// `orientation` is the complete attitude and replaces the motion sensors.
    static constexpr std::uint32_t OrientationValid = 1u << 2;
    /// The player asks for the direction the controller points in now to count as straight
    /// ahead (for when nothing but the motion sensors knows its heading).
    static constexpr std::uint32_t RecenterYaw = 1u << 3;
    /// `position` is not where the controller is but where it should be assumed to be while
    /// nothing sees it, relative to the head (for a player who chooses between holding it up
    /// in view and resting it in the lap).
    static constexpr std::uint32_t AssumedOffset = 1u << 4;
    /// The player asks for the view to be reset: where the head is now is where they sit, and
    /// the way it faces is straight ahead. (The message needs nothing else to be valid.)
    static constexpr std::uint32_t RecenterSeat = 1u << 5;
    /// `orientation` is not the controller's own but what the hands holding it give away of
    /// how it is turned and tilted: it stands in for motion sensors the controller does not
    /// have, and `position` is taken as it is without `OrientationValid`.
    static constexpr std::uint32_t HeldInHands = 1u << 6;
    /// The player asks to be turned round one step where they sit, to the left or to the
    /// right (see Runtime::TurnView). (The message needs nothing else to be valid.)
    static constexpr std::uint32_t TurnLeft = 1u << 7;
    static constexpr std::uint32_t TurnRight = 1u << 8;

    Header header{.type = MessageType::PadPose};
    std::uint32_t flags{};
    float position[3]{};
    float linear_velocity[3]{};
    float yaw{};
    float orientation[4]{0.0f, 0.0f, 0.0f, 1.0f};
    float angular_velocity[3]{};
};

/// Emulator to host: what the title asks of the controller.
struct PadFeedback {
    Header header{.type = MessageType::PadFeedback};
    /// Strength of the high and the low frequency motor, 0 to 255.
    std::uint8_t small_motor{};
    std::uint8_t large_motor{};
    /// Colour of the light bar.
    std::uint8_t red{};
    std::uint8_t green{};
    std::uint8_t blue{};
    std::uint8_t reserved[3]{};
};

} // namespace Core::Vr::Protocol
