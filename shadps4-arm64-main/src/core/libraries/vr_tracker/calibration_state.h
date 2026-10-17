// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <chrono>

namespace Libraries::VrTracker {

// The host already supplies calibrated poses, but a guest can wait for an observable
// CALIBRATING -> TRACKING transition after requesting recalibration. Keep the first
// state visible briefly, starting when it is actually polled (not when requested).
class CalibrationState {
public:
    using Clock = std::chrono::steady_clock;

    void Request() {
        pending = true;
        observed = false;
    }

    bool IsCalibrating(Clock::time_point now) {
        if (!pending) {
            return false;
        }
        if (!observed) {
            observed = true;
            ready_at = now + std::chrono::milliseconds{100};
        }
        if (now < ready_at) {
            return true;
        }
        pending = false;
        return false;
    }

private:
    bool pending{};
    bool observed{};
    Clock::time_point ready_at{};
};

} // namespace Libraries::VrTracker
