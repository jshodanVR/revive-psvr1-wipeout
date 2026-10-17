// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <atomic>
#include <array>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <string>

#include "common/types.h"

namespace Core::Vr {

struct Vec3 {
    float x{};
    float y{};
    float z{};
};

struct Quat {
    float x{};
    float y{};
    float z{};
    float w{1.0f};
};

struct Pose {
    Vec3 position;
    Quat orientation;
};

Quat Multiply(const Quat& a, const Quat& b);
Quat Conjugate(const Quat& q);
inline Quat Normalize(const Quat& q) {
    const float length = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
    if (length < 1e-6f) {
        return {};
    }
    return {q.x / length, q.y / length, q.z / length, q.w / length};
}

inline Vec3 Rotate(const Quat& q, const Vec3& v) {
    const Vec3 u{q.x, q.y, q.z};
    const Vec3 t{
        u.y * v.z - u.z * v.y + q.w * v.x,
        u.z * v.x - u.x * v.z + q.w * v.y,
        u.x * v.y - u.y * v.x + q.w * v.z,
    };
    return {
        v.x + 2.0f * (u.y * t.z - u.z * t.y),
        v.y + 2.0f * (u.z * t.x - u.x * t.z),
        v.z + 2.0f * (u.x * t.y - u.y * t.x),
    };
}

Quat FromYawPitch(float yaw, float pitch);
/// Yaw about +Y, then pitch about +X, then roll about +Z, in radians.
Quat FromYawPitchRoll(float yaw, float pitch, float roll);
/// Keeps only the rotation about the vertical axis.
Quat YawOnly(const Quat& q);

/// Per-eye field of view as tangents of the half angles. "out" is the temple side and "in" the
/// nose side, so one set of values describes both eyes (this is how libSceHmd reports it).
struct Fov {
    float tan_out{1.20743f};
    float tan_in{1.181346f};
    float tan_top{1.262872f};
    float tan_bottom{1.262872f};
};

struct HeadsetIdentity {
    std::string runtime;
    std::string system;
    u32 vendor_id{};

    bool operator==(const HeadsetIdentity&) const = default;
};

/// A tracked device sample, expressed in PSVR tracker space: metres, +X right, +Y up and
/// -Z towards the camera the player is facing.
struct DeviceState {
    Pose pose;
    Vec3 linear_velocity;
    Vec3 angular_velocity;
    u64 sequence{}; ///< Host sample counter, 0 when the host never reported this device.
    bool tracked{};
};

struct MoveState {
    DeviceState device;
    u16 buttons{};
    u16 trigger{};
    bool connected{};
    std::chrono::steady_clock::time_point sampled{};
};

/// What a title asks of the controller: its two rumble motors and the colour of its light.
struct PadFeedback {
    u8 small_motor{};
    u8 large_motor{};
    u8 red{};
    u8 green{};
    u8 blue{};

    bool operator==(const PadFeedback&) const = default;
};

/// Everything the host needs to put one emulated HMD frame in front of the user's eyes.
struct PresentedFrame {
    u32 id{};         ///< Matches the marker the presenter stamps into the frame.
    Pose render_pose; ///< Head pose the guest rendered this frame with, in host space.
    Fov fov;          ///< Field of view the eye images were rendered with.
    u32 eye_width{};
    u32 eye_height{};
};

struct Config {
    bool headset_connected{false};
    float ipd{0.063f};
    Fov fov{};
    /// The field of view the title is told of is the headset's own (as the host found it), not
    /// a PlayStation VR's; fov_scale applies to either.
    bool fov_from_headset{false};
    /// Use matching render bounds for both eyes, enclosing the detected headset frusta.
    /// This preserves binocular coverage when a standalone host narrows the view.
    bool fov_symmetric{false};
    float fov_scale{1.0f};
    /// Where the player's resting head position sits in tracker space. The PS Camera is the
    /// origin, so the player is placed a comfortable distance in front of it.
    Vec3 origin_offset{0.0f, 0.0f, 1.5f};
    /// Refresh rate the emulated headset panel runs at. Astro Bot renders 60 fps reprojected
    /// to 120 Hz.
    u32 refresh_rate{120};
    /// Sweeps the head when no host is feeding poses, so stereo output can be checked on a
    /// desktop build.
    bool demo_motion{false};
    /// Where a controller is held when the host cannot see it: relative to the player's head
    /// position, or to the resting head position when it does not follow the head. A gamepad has
    /// no positional tracking outside of the PS Camera, so this is the normal case. The default
    /// is an arm's reach in front of the chest, which is also where titles ask for the controller
    /// to be presented when they calibrate.
    Vec3 pad_offset{0.0f, -0.17f, -0.50f};
    bool pad_follows_head{true};
};

class Runtime {
public:
    static Runtime& Instance();

    /// Decides whether a virtual headset is plugged in for the title being booted.
    void Configure(bool psvr_supported, bool psvr_required);

    bool IsHeadsetConnected() const {
        return config.headset_connected;
    }
    const Config& GetConfig() const {
        return config;
    }

    /// Whether the headset is on the player's head. A host that can tell says so; a title asks
    /// (the headset has a sensor for it) and waits for the player while it is not.
    void SetHeadsetWorn(bool worn) {
        headset_worn.store(worn, std::memory_order_relaxed);
    }
    bool IsHeadsetWorn() const {
        return headset_worn.load(std::memory_order_relaxed);
    }

    // Host side. Poses arrive in host space: the headset's own, whose origin and heading are
    // wherever its system last put them. Where the player actually sits in it, and which way
    // they face, is the "seat" (see RecenterSeat); the title is given poses relative to that.
    void UpdateHead(const DeviceState& host_state);
    /// Declares the head's present position to be the resting one and the direction it looks
    /// in to be straight ahead. A title places its world around where the player sat when it
    /// last took stock (PlayStation VR: at its start, and when the player holds OPTIONS), and
    /// expects them to face the camera that tracks them; a player who has since moved, or who
    /// faces another way than the headset's system assumes, sits in the wrong spot of that
    /// world. Happens by itself for the first pose and when the title first asks for one.
    void RecenterSeat();
    /// RecenterSeat, and the title is told to take stock again, the way the console tells it
    /// when the player asks for the view to be reset.
    /// Optionally make the currently tracked virtual pad level and centred too.
    /// Its current physical pose becomes neutral; later 6DoF motion is preserved.
    void RequestRecenter(bool neutral_tracked_pad = false);
    /// Turns the player round where they sit, by so many steps to the right (to the left if
    /// negative): for those who cannot turn round themselves. The head stays where it is in
    /// the title's world and faces another way; a controller that nothing locates comes
    /// along. The title is told nothing: to it the player has turned. A reset of the view
    /// faces them straight ahead again. (SHADPS4_VR_TURN=<degrees> is the step: 30, 0 for none.)
    void TurnView(int steps);
    /// For a host whose poses are counted from the seat already (scripted tests): the seat is
    /// the origin of its space and stays there until RecenterSeat is called.
    void FixSeat();
    void UpdatePad(const DeviceState& host_state);
    /// The host that said where the controller is and how it is turned (UpdatePad) no longer
    /// does: it is placed by what else is known of it again.
    void ReleasePad();
    /// For hosts that only know how the controller is turned (from its motion sensors) and not
    /// where it is. The runtime then places it with Config::pad_offset.
    void UpdatePadOrientation(const Quat& orientation, const Vec3& angular_velocity);
    /// The same for a host that works out how the controller is turned from what it sees of
    /// it (the hands holding a controller without motion sensors), in its own space.
    void UpdatePadHeldOrientation(const Quat& host_orientation);
    /// The controller's own motion sensors, for hosts that have nothing better. Readings are in
    /// the controller's frame (+X right, +Y out of the face buttons, +Z towards the player), in
    /// rad/s and m/s² with gravity included, as SDL reports them. The runtime works out how the
    /// controller is turned from them.
    void UpdatePadGyro(const Vec3& angular_velocity);
    void UpdatePadAcceleration(const Vec3& acceleration);
    /// Declares the direction the controller points in right now to be straight ahead.
    void ResetPadYaw();
    /// Whether the controller's motion sensors ever said how it is held.
    bool PadMotionKnown() const {
        std::scoped_lock lock{mutex};
        return pad_attitude_valid;
    }

    /// What a player does with the controller about the view itself, where no application
    /// around the emulator sees to it: the first press of X once the headset shows the game
    /// takes where the head is then for the player's seat (they have settled, controller in
    /// hand); OPTIONS held for a second resets the view, as on a PlayStation VR, and so does
    /// the PS button. The buttons still reach the title.
    enum class PadButton { Cross, Options, Home };
    void EnableViewGestures(bool enabled);
    void NotePadButton(PadButton button, bool pressed);
    /// To be called often: holding a button takes time.
    void PollViewGestures();
    /// For hosts that see where the controller is held (the hands around it) but not how it is
    /// turned; the motion sensors keep providing that.
    void UpdatePadPosition(const Vec3& host_position, const Vec3& linear_velocity);
    /// The host lost sight of the controller: it stays where it was last seen, relative to
    /// the player (or where Config::pad_offset puts it if it never was seen).
    void ClearPadPosition();
    /// Replaces Config::pad_offset, and forgets where the controller was last seen.
    void SetPadOffset(const Vec3& offset);
    /// A controller that nothing locates is held to be at a fixed place before the player:
    /// the standard one (Config::pad_offset, which is where the title looks for it when it
    /// starts) or one of the player's own choosing, for wherever the standard one is in the
    /// way of the view or out of reach of what the controller is to be held to. The player's
    /// own place is kept in the user folder; every start begins at the standard one.
    /// Moves the player's own place by so many metres (right, up, towards the player), and
    /// puts the controller there.
    void MoveOwnPadPlace(const Vec3& by);
    /// From the standard place to the player's own, or back.
    void SwitchPadPlace();
    /// What the host can tell about the controller's heading (0 = straight ahead, positive to
    /// the left). The attitude worked out from the motion sensors is pulled towards it, which
    /// takes out the drift a gyroscope has about the vertical.
    void UpdatePadYawReference(float yaw);

    // The other direction: what the title wants the real controller to do.
    void SetPadVibration(u8 small_motor, u8 large_motor);
    void SetPadLight(u8 red, u8 green, u8 blue);
    void SetPadFeedbackListener(std::function<void(const PadFeedback&)> listener);
    void UpdateOptics(const Fov& fov, float ipd);
    void SetHeadsetIdentity(const HeadsetIdentity& identity);
    /// What the host's headset shows of the world, both eyes together (the widest of the two to
    /// every side), as soon as it knows. Kept in the user folder for later starts.
    void NoteHeadsetFov(const Fov& fov);
    /// The field of view the title is told its headset has (sceHmdGetFieldOfView: titles ask
    /// once, when they open the headset). With Config::fov_from_headset that is the host
    /// headset's own, which is waited for a while if the host does not know it yet.
    Fov TitleFov();
    /// The host's display refreshed (see Protocol::Refresh), at `time` nanoseconds of the
    /// steady clock, or just now if that is 0.
    void NoteDisplayRefresh(float rate, u64 time);
    struct DisplayRefresh {
        u64 sequence{}; ///< How many the host has told of, 0 for none.
        std::chrono::steady_clock::time_point time;
        float rate{};
    };
    DisplayRefresh GetDisplayRefresh() const;
    /// How often the emulated headset refreshes: as often as the host's display while the host
    /// tells of its refreshes, as often as Config::refresh_rate says otherwise.
    float HeadsetRefreshRate() const;

    // Guest side, tracker space.
    DeviceState GetHead();
    DeviceState GetPad();
    // Experimental, explicitly enabled with SHADPS4_EMULATE_MOVE=1.
    bool MovesEnabled() const;
    void UpdateMove(u32 hand, const DeviceState& host, u16 buttons, u16 trigger, bool connected);
    MoveState GetMove(u32 hand) const;
    void ReleaseMoves();
    bool AimEnabled() const;
    void UpdateAim(const DeviceState& rear, const DeviceState& support, bool support_gripped);
    DeviceState GetAim() const;
    void ReleaseAim();

    /// Converts a tracker-space pose handed back by the guest into host space.
    Pose ToHostSpace(const Pose& tracker_pose) const;

    u32 NextFrameId() {
        return ++frame_counter;
    }
    void SetFrameListener(std::function<void(const PresentedFrame&)> listener);
    void NotifyFramePresented(const PresentedFrame& frame);

private:
    Runtime();

    DeviceState DemoHead() const;
    void RecenterSeatLocked();
    void PlaceHead();
    void PlacePad();
    Vec3 PositionToTracker(const Vec3& host) const;
    Vec3 DirectionToTracker(const Vec3& host) const;

    Config config;
    std::atomic<bool> headset_worn{true};
    std::atomic<bool> view_gestures{};
    std::mutex gesture_mutex;
    bool gesture_seat_taken{};
    bool gesture_options_down{};
    bool gesture_options_fired{};
    std::chrono::steady_clock::time_point gesture_options_since;
    mutable std::mutex mutex;
    DeviceState head;
    // What the host last said of the head, in its own space, and the seat in that space.
    DeviceState host_head;
    // Where the head was a moment ago, and how fast that says it moves.
    Vec3 head_seen_position;
    std::chrono::steady_clock::time_point head_seen_time;
    bool head_seen{};
    Vec3 head_speed;
    Vec3 seat_position;
    Quat seat_yaw;
    bool seat_valid{};
    // The seat before the last reset of the view, for the frames that were drawn from it and
    // are only shown after.
    Vec3 previous_seat_position;
    Quat previous_seat_yaw;
    // How far TurnView has turned the player since the view was last reset, to the left.
    float view_turn{};
    std::chrono::steady_clock::time_point seat_changed;
    bool title_asked{};
    DeviceState pad;
    DeviceState host_pad;
    // A controller-local rotation and host-space translation, applied only to
    // fully tracked pad samples. Physical gamepad sensor input is unaffected.
    Quat tracked_pad_rotation;
    Vec3 tracked_pad_translation;
    std::array<MoveState, 2> moves;
    DeviceState aim;
    std::chrono::steady_clock::time_point aim_sampled;
    bool pad_position_tracked{};
    // When a host last said where the controller is and how it is turned (UpdatePad).
    std::chrono::steady_clock::time_point pad_host_pose_time;
    // Smoothed point the untracked controller hangs off.
    Vec3 pad_anchor;
    bool pad_anchor_valid{};
    // The player's own place for it, from that point, and whether it is there.
    Vec3 own_pad_offset{0.0f, -0.30f, -0.45f};
    bool own_pad_place{};
    std::chrono::steady_clock::time_point pad_anchor_time;
    // Attitude estimated from the controller's motion sensors.
    Quat pad_attitude;
    Vec3 pad_acceleration;
    bool pad_acceleration_valid{};
    bool pad_attitude_valid{};
    std::chrono::steady_clock::time_point pad_motion_time;
    // Where the host last saw the controller, in tracker space.
    Vec3 pad_seen_position;
    Vec3 pad_seen_velocity;
    bool pad_seen{};
    // Where it was last seen relative to the anchor, to stay there once out of sight.
    Vec3 pad_seen_offset;
    bool pad_seen_offset_valid{};
    std::chrono::steady_clock::time_point pad_seen_time;
    // Where the controller is reported to be; follows the above without jumping.
    Vec3 pad_shown_position;
    bool pad_shown_valid{};
    std::chrono::steady_clock::time_point pad_shown_time;
    float pad_yaw_reference{};
    bool pad_yaw_reference_valid{};
    std::chrono::steady_clock::time_point pad_yaw_reference_time;
    DisplayRefresh display_refresh;
    // The host headset's field of view, once known.
    std::condition_variable headset_fov_known;
    HeadsetIdentity headset_identity;
    Fov headset_fov;
    bool has_headset_fov{};
    PadFeedback pad_feedback;
    std::function<void(const PadFeedback&)> pad_feedback_listener;
    std::function<void(const PresentedFrame&)> frame_listener;
    u32 frame_counter{};
};

} // namespace Core::Vr
