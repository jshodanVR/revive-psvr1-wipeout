// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cmath>

namespace Input {

/// Turning the view by steps, for players who sit where they cannot turn round: a title made
/// for a headset has things behind the player, and expects them to look. A button the title
/// has no use for in play is held, and each flick of the right stick to a side then turns the
/// view one step that way (Core::Vr::Runtime::TurnView) instead of moving a finger over the
/// touchpad.
class ViewTurn {
public:
    /// How far to a side the stick has to go, and how near the middle it has to come back
    /// before it can turn again.
    static constexpr float Far = 0.7f;
    static constexpr float Near = 0.35f;

    /// `held`: the button. `x`: the stick, -1 at its left end to 1 at its right.
    /// 1: a step to the right now, -1: one to the left, 0: nothing.
    int Update(bool held, float x) {
        if (!held) {
            ready = false;
            return 0;
        }
        // (A stick that is at a side already when the button comes down turns nothing.)
        if (std::abs(x) < Near) {
            ready = true;
            return 0;
        }
        if (ready && std::abs(x) > Far) {
            ready = false;
            return x > 0.0f ? 1 : -1;
        }
        return 0;
    }

    void Reset() {
        ready = false;
    }

private:
    bool ready{};
};

} // namespace Input
