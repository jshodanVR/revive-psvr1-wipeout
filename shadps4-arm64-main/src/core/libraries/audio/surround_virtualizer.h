// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <vector>

#include "common/types.h"
#include "core/libraries/audio3d/audio3d_spatializer.h"

namespace Libraries::AudioOut {

/// Turns 7.1 speaker audio into two channels for headphones (or the speakers of a headset, which
/// sit next to the ears) by placing seven virtual speakers around the listener. A title that mixes
/// for the head pose it reads from the headset keeps its sense of direction that way; folding the
/// channels down to left and right would lose everything but left and right.
class SurroundVirtualizer {
public:
    /// `layout` is a port's channel layout: for front left, front right, centre, LFE and the
    /// two surround pairs, the index of the channel in an input frame.
    explicit SurroundVirtualizer(const std::array<int, 8>& layout, u32 sample_rate);

    /// Renders `frames` frames of eight interleaved samples into interleaved stereo.
    void Process(const float* input, u32 frames, float* output);
    void Process(const s16* input, u32 frames, float* output);

private:
    void Render(u32 frames, float* output);

    static constexpr u32 NumSpeakers = 7;

    std::array<int, 8> layout;
    float sample_rate;
    std::array<Audio3d::Spatializer, NumSpeakers> speakers;
    std::array<std::vector<float>, 8> planar;
    float bass_state{};
    float limiter_gain{1.0f};
};

} // namespace Libraries::AudioOut
