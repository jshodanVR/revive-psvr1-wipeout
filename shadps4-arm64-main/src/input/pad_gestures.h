// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "moved_finger.h"

namespace Input {

/// What a finger does on a DualShock 4's touchpad, done by buttons: for controllers that
/// have no touchpad (other gamepads, a headset's own controllers), where a title made for
/// one wants more of it than a stick's finger does comfortably. Three things cover what
/// ASTRO BOT Rescue Mission asks of the pad:
///  - pressing it, for as long as a button is held (its water cannon and its machine gun
///    fire while the pad is pressed);
///  - a swipe forward, once for each press of a button (its hook is shot that way, its
///    throwing stars thrown, its chests opened);
///  - a pull back that is held for as long as a button is, and let go when the button is
///    (the catapult at the end of each of its levels, and pulling on the hook's rope).
/// Each is a finger a title can follow (see MovedFinger): it is where it lands for long
/// enough to be seen there, and it is lifted for long enough to be seen lifted before it
/// lands again.
class PadGestures {
public:
    struct Controls {
        bool press{};
        bool swipe{};
        bool pull{};
    };

    struct Touch {
        bool down{};
        /// On the pad, 0 to 1: from its left to its right, from its far edge to its near one.
        float x{0.5f};
        float y{0.5f};
        /// The pad itself is pressed in.
        bool pressed{};
    };

    /// `time` in seconds, from any moment. To be called at least some thirty times a second,
    /// whether a button is held or not.
    Touch Update(double time, const Controls& controls) {
        if (controls.swipe && !was.swipe && swipes < 2) {
            // (One that is asked for while another is under way comes after it.)
            ++swipes;
        }
        was = controls;

        switch (doing) {
        case Doing::Nothing:
            if (time - lifted < Apart) {
                break;
            }
            if (controls.press) {
                doing = Doing::Press;
                finger.Land(time, 0.5f, 0.5f);
            } else if (swipes != 0) {
                --swipes;
                doing = Doing::Swipe;
                finger.Land(time, 0.5f, SwipeFrom);
            } else if (controls.pull) {
                doing = Doing::Pull;
                let_go = false;
                finger.Land(time, 0.5f, PullFrom);
            }
            break;
        case Doing::Press:
            if (!controls.press && finger.Down(time) >= ShortestPress) {
                Lift(time);
            }
            break;
        case Doing::Swipe:
            if (finger.Follow(time, 0.5f, SwipeTo)) {
                Lift(time);
            }
            break;
        case Doing::Pull: {
            // Once let go it is let go, however soon the button is pulled again: the way
            // back is finished first, so that a quick pull of the button is a whole pull.
            let_go = let_go || !controls.pull;
            const bool there = finger.Follow(time, 0.5f, PullTo);
            if (let_go && there && finger.Down(time) >= ShortestPull) {
                Lift(time);
            }
            break;
        }
        }
        return {doing != Doing::Nothing, finger.x, finger.y, doing == Doing::Press};
    }

    /// Whatever was under way is over, and the finger is off the pad.
    void Reset() {
        doing = Doing::Nothing;
        swipes = 0;
        was = {};
    }

    bool IsActive() const {
        return doing != Doing::Nothing;
    }

    /// Where a swipe forward begins and ends, of the pad's depth from its far edge.
    static constexpr float SwipeFrom = 0.80f;
    static constexpr float SwipeTo = 0.12f;
    /// Where a pull back begins and ends.
    static constexpr float PullFrom = 0.20f;
    static constexpr float PullTo = 0.97f;
    /// The pad stays pressed this long at least, in seconds, however briefly the button was.
    static constexpr double ShortestPress = 0.12;
    /// A pull lasts this long at least. (That catapult wants more than a quarter second.)
    static constexpr double ShortestPull = 0.36;
    /// The finger is off the pad this long at least between two things.
    static constexpr double Apart = 0.08;

private:
    enum class Doing { Nothing, Press, Swipe, Pull };

    void Lift(double time) {
        doing = Doing::Nothing;
        lifted = time;
    }

    Doing doing{Doing::Nothing};
    Controls was;
    int swipes{};
    bool let_go{};
    double lifted{-1.0};
    MovedFinger finger;
};

} // namespace Input
