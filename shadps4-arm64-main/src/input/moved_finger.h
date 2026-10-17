// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cmath>

namespace Input {

/// A finger on a touchpad that something other than a finger moves (a stick, a button), in
/// such a way that a title sees what a finger would have shown it.
///
/// A title reads its controller once for every frame it draws, and makes what it makes of a
/// touch from what it saw then: where the finger was when it first saw it down, where when
/// it last did, how far it got from one frame to the next. A real finger takes a tenth of a
/// second and more over a swipe; a stick is at its end within a few hundredths, a button at
/// once. A finger that lands and is at the far end of its way before the title has looked
/// even once (at thirty frames a second it looks every 33 ms) never moved at all as far as
/// the title can tell: ASTRO BOT Rescue Mission would not shoot its catapult for a stick
/// pulled back quickly, because the pull began, for it, where it ended.
///
/// So this finger stays where it lands until the title has seen it there, goes to where it
/// is wanted no faster than a finger flicks, and is only done once the title has seen it at
/// its end as well.
struct MovedFinger {
    /// How long it stays where it landed, in seconds: three frames of a title that draws
    /// thirty a second.
    static constexpr double Dwell = 0.09;
    /// How fast it goes, in widths of the pad a second: a brisk flick.
    static constexpr float Speed = 6.0f;
    /// How long it has to have been at its goal to count as arrived, in seconds.
    static constexpr double Hold = 0.08;

    /// On the pad, 0 to 1: from its left to its right, from its far edge to its near one.
    float x{0.5f};
    float y{0.5f};

    void Land(double time, float at_x, float at_y) {
        x = at_x;
        y = at_y;
        landed = time;
        moved = time;
        arrived = false;
    }

    /// Goes on towards a goal, which may move. True once it has been there for `Hold`.
    bool Follow(double time, float goal_x, float goal_y) {
        const double passed = time - moved;
        moved = time;
        if (time - landed < Dwell) {
            return false;
        }
        const float dx = goal_x - x;
        const float dy = goal_y - y;
        const float distance = std::hypot(dx, dy);
        // (Called too seldom, it does not leap to make up for it.)
        const float step =
            Speed * static_cast<float>(passed < 0.0 ? 0.0 : passed > 0.05 ? 0.05 : passed);
        if (distance > step && distance > 1e-4f) {
            x += dx / distance * step;
            y += dy / distance * step;
            arrived = false;
            return false;
        }
        x = goal_x;
        y = goal_y;
        if (!arrived) {
            arrived = true;
            arrived_at = time;
        }
        return time - arrived_at >= Hold;
    }

    /// How long ago it landed.
    double Down(double time) const {
        return time - landed;
    }

private:
    double landed{};
    double moved{};
    bool arrived{};
    double arrived_at{};
};

} // namespace Input
