// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>

#include "common/types.h"

namespace Libraries::Audio3d {

/// Where a sound object is and how loud, as the title last described it. Positions are relative
/// to the listener's head: +X to the right, +Y up, +Z behind.
struct ObjectPlacement {
    float x{};
    float y{};
    float z{};
    float gain{};
    /// 0 for a point source up to 2 pi for a sound that comes from everywhere.
    float spread{};
};

/// Renders a mono sound for headphones so that it is heard from a direction: each ear gets the
/// sound with the delay and the treble loss the head causes for that direction (the spherical
/// head model of Brown and Duda), and sounds from behind are dulled a little.
class Spatializer {
public:
    static constexpr u32 MaxBlock = 2048;

    void Reset();

    /// Adds `count` samples of `in` to the interleaved stereo `out`.
    void Process(const float* in, u32 count, float* out, const ObjectPlacement& placement,
                 float sample_rate);

private:
    struct Ear {
        float delay{};   ///< samples
        float treble{1}; ///< gain far above the head's corner frequency
        float gain{};
    };
    struct EarState {
        float previous_input{};
        float previous_output{};
    };

    static constexpr u32 History = 64;

    std::array<float, History> history{};
    std::array<Ear, 2> ears{};
    std::array<EarState, 2> filters{};
    float rear{};
    float rear_state[2]{};
    bool primed{};
};

} // namespace Libraries::Audio3d
