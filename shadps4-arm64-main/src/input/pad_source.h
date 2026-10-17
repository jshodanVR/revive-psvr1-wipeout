// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <mutex>

namespace Input {

/// In a headset on a PC, the first player's controller can be played by two things at once: a
/// gamepad connected to the PC, and the headset's own controllers. Whichever was used last
/// plays; the other is left aside until it is used again. (A gamepad that only lies on the
/// desk kept the headset's controllers from doing anything at all before.)
///
/// One case is left as it was: a "gamepad" that a streaming program makes of the headset's
/// controllers themselves (Virtual Desktop can) says what they say, when they say it. Then
/// it is the gamepad that plays: a use of the headset's controllers only counts once the
/// gamepad has had nothing to say around the same time.
///
/// Times are in seconds of any one steady clock.
class PadSource {
public:
    /// How long a use of the headset's controllers waits for the gamepad not to say the same.
    static constexpr double Echo = 0.25;

    /// A button of the gamepad was pressed, a stick or a trigger of it pushed, its touchpad
    /// touched. True when that takes the controller back from the headset's controllers.
    bool GamepadUsed(double now) {
        std::scoped_lock lock{mutex};
        gamepad_used = now;
        gamepad_ever = true;
        asked = -1.0;
        if (!headset) {
            return false;
        }
        headset = false;
        return true;
    }

    /// Once for every frame of the headset. `gamepad`: the PC has a gamepad for the first
    /// player. `used`: one of the headset's controllers was used just now (a button that plays
    /// pressed, a stick pushed or pressed in: not what a hand does by resting on it).
    /// Whether the headset's controllers play.
    bool Update(double now, bool gamepad, bool used) {
        std::scoped_lock lock{mutex};
        if (!gamepad) {
            // Nothing else is there to play. (And a gamepad that comes later waits to be used.)
            headset = true;
            gamepad_pose = false;
            asked = -1.0;
            return true;
        }
        if (headset) {
            return true;
        }
        if (used && asked < 0.0) {
            asked = now;
        }
        if (asked >= 0.0) {
            if (gamepad_ever && gamepad_used >= asked - Echo) {
                asked = -1.0;
            } else if (now - asked >= Echo) {
                asked = -1.0;
                headset = true;
                gamepad_pose = false;
            }
        }
        return headset;
    }

    bool HeadsetPlays() const {
        std::scoped_lock lock{mutex};
        return headset;
    }

    // Buttons may be played on the PC while VR supplies a six-degree pose.
    // Deliberately turning the active physical pad requests its own motion again.
    // Resting sensor noise must neither take over nor disable the tracked hands.
    bool GamepadMotionUsed(float angular_speed) {
        std::scoped_lock lock{mutex};
        if (headset || gamepad_pose || !(angular_speed > 0.35f)) {
            return false;
        }
        gamepad_pose = true;
        return true;
    }

    bool GamepadPosePlays() const {
        std::scoped_lock lock{mutex};
        return gamepad_pose;
    }

private:
    mutable std::mutex mutex;
    bool headset{};
    bool gamepad_pose{};
    bool gamepad_ever{};
    double gamepad_used{};
    double asked{-1.0};
};

/// The one for the first player's controller.
inline PadSource& FirstPadSource() {
    static PadSource source;
    return source;
}

} // namespace Input
