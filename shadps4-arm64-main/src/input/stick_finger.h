// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <cmath>
#include <cstddef>

#include "moved_finger.h"

namespace Input {

/// A finger on a touchpad, moved by a stick: for controllers that have no touchpad, where a
/// title made for one has its players swipe and drag. Where the stick points is where the
/// finger goes, with what a stick needs and a finger does not:
///  - The finger comes down behind the middle of the pad, on the side the stick comes from,
///    and is dragged from there to where the stick points: a push of the stick is a swipe
///    over all of the way it goes, and more.
///  - It is a finger a title can follow (see MovedFinger): it stays where it came down
///    until the title has seen it there, and goes no faster than a finger flicks. A stick
///    is at its end before a title that draws thirty frames a second has looked once.
///  - A stick that is let go flies back to its centre; a finger that is lifted does not go
///    back to where it came from first. A title that goes by how far the finger was dragged
///    when it lifts (ASTRO BOT Rescue Mission pulls a catapult back that way at the end of
///    every level, and shoots when the finger lets go) would find nothing pulled at all. So
///    the finger goes to where the stick was before it started back, and lifts from there
///    once the stick has come to rest, however briefly the stick was out.
///  - A stick may be moved through its centre, from one side to the other, without the finger
///    lifting on the way: a drag over the whole pad.
///  - The stick does not have to be pushed all the way for the finger to reach the pad's edge,
///    and the finger stays down for a moment at least, however briefly the stick was pushed.
///    (That catapult only shoots for a finger that was down for more than a quarter of a
///    second and dragged over more than four tenths of the pad.)
///  - A stick that never comes to rest near its centre (a worn one that drifts) moves no
///    finger: it would touch the pad for good, and a real finger on a real touchpad would
///    count for nothing beside it.
class StickFinger {
public:
    struct Touch {
        bool down;
        /// On the pad, 0 to 1: from its left to its right, from its far edge to its near one.
        float x;
        float y;
    };

    /// `time` in seconds, from any moment; `x` to the right and `y` towards the player of the
    /// stick's centre, -1 to 1 each. To be called at least some thirty times a second.
    Touch Update(double time, float x, float y) {
        const float r = std::hypot(x, y);
        const float previous_r = count != 0 ? At(0).r : r;
        Push({time, x, y, r});

        if (!down) {
            if (r <= Rest) {
                armed = true;
            }
            if (!armed || r <= Start) {
                return Lifted();
            }
            down = true;
            armed = false;
            held = false;
            goal_x = x;
            goal_y = y;
            finger.Land(time, 0.5f - x / r * Back, 0.5f - y / r * Back);
            return Touching();
        }

        // The furthest the stick has been out in the last moments, and whether it is on its
        // way back from there in a hurry.
        const Sample* peak = &At(0);
        for (size_t back = 1; back < count && time - At(back).time <= Window; ++back) {
            if (At(back).r > peak->r) {
                peak = &At(back);
            }
        }
        const bool flying_back = r < peak->r - Drop && r <= previous_r + 0.01f;
        const bool inside = r <= Start;
        if (!inside && !flying_back) {
            goal_x = x;
            goal_y = y;
            held = false;
        } else if (flying_back && !held) {
            goal_x = peak->x;
            goal_y = peak->y;
            held = true;
        }
        const bool there = finger.Follow(time, OnPad(goal_x), OnPad(goal_y));
        if (inside && IsStill(time) && there && finger.Down(time) >= ShortestTouch) {
            down = false;
            armed = r <= Rest;
            return Lifted();
        }
        return Touching();
    }

    /// The finger is lifted, and the stick has to come to rest before it touches again.
    void Reset() {
        down = false;
        armed = false;
        held = false;
        count = 0;
    }

    bool IsDown() const {
        return down;
    }

    /// The stick counts as at rest within this of its centre, and the finger comes down once
    /// it is further out than that.
    static constexpr float Rest = 0.15f;
    static constexpr float Start = 0.25f;
    /// How far behind the pad's middle the finger comes down, of the pad's size.
    static constexpr float Back = 0.25f;
    /// How far from the pad's centre the stick would put the finger if the pad went on, of
    /// the pad's size: the finger is at the pad's edge with the stick four fifths out.
    static constexpr float Reach = 0.62f;
    /// A touch lasts this long at least, in seconds.
    static constexpr double ShortestTouch = 0.36;

private:
    struct Sample {
        double time;
        float x;
        float y;
        float r;
    };

    /// Flying back: nearer the centre by this much than the stick was within that long.
    static constexpr float Drop = 0.12f;
    static constexpr double Window = 0.045;
    /// Come to rest: near the centre and hardly moved for this long.
    static constexpr double Still = 0.025;
    static constexpr float StillMove = 0.05f;

    /// The sample taken `back` samples ago, 0 for the newest.
    const Sample& At(size_t back) const {
        return samples[(newest + samples.size() - back) % samples.size()];
    }

    void Push(const Sample& sample) {
        newest = (newest + 1) % samples.size();
        samples[newest] = sample;
        if (count < samples.size()) {
            ++count;
        }
    }

    bool IsStill(double time) const {
        const Sample& now = At(0);
        for (size_t back = 1; back < count; ++back) {
            const Sample& then = At(back);
            if (std::abs(then.x - now.x) > StillMove || std::abs(then.y - now.y) > StillMove) {
                return false;
            }
            if (time - then.time >= Still) {
                return true;
            }
        }
        return false;
    }

    /// Where on the pad the stick, that far out, wants the finger.
    static float OnPad(float value) {
        const float at = 0.5f + value * Reach;
        return at < 0.0f ? 0.0f : at > 1.0f ? 1.0f : at;
    }

    Touch Touching() const {
        return {true, finger.x, finger.y};
    }

    Touch Lifted() const {
        return {false, finger.x, finger.y};
    }

    std::array<Sample, 32> samples{};
    size_t newest{};
    size_t count{};
    bool down{};
    bool armed{};
    bool held{};
    float goal_x{};
    float goal_y{};
    MovedFinger finger;
};

} // namespace Input
