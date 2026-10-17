// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <cmath>

#include "core/libraries/audio/surround_virtualizer.h"

namespace Libraries::AudioOut {

namespace {

constexpr float Pi = 3.14159265358979323846f;

// Where the speakers of a 7.1 room stand, as degrees to the right of straight ahead: front
// left and right, centre, the surround pair at the sides and the pair behind.
//
// The layout tables call the pair that follows the LFE "back" and the last pair "side". How
// titles pan says otherwise: a sound 80 degrees to the right comes out of the front right
// speaker and, louder, the first of those pairs, hardly from the second. The first pair is the
// 5.1 surround pair at about 110 degrees, the second the one 7.1 adds behind the listener.
constexpr std::array<float, 7> SpeakerAngle = {-30.0f, 30.0f,   0.0f,  -110.0f,
                                               110.0f, -150.0f, 150.0f};
// The matching positions in an input layout (which also has the LFE at index 3).
constexpr std::array<u32, 7> SpeakerSlot = {0, 1, 2, 4, 5, 6, 7};
constexpr u32 LfeSlot = 3;

// Seven speakers playing at once are louder than any one of them.
constexpr float Headroom = 0.8f;
constexpr float LfeLevel = 0.5f;
constexpr float LfeCutoff = 160.0f;

} // namespace

SurroundVirtualizer::SurroundVirtualizer(const std::array<int, 8>& layout_, u32 sample_rate_)
    : layout{layout_}, sample_rate{static_cast<float>(sample_rate_)} {
    for (auto& index : layout) {
        index = std::clamp(index, 0, 7);
    }
}

void SurroundVirtualizer::Process(const float* input, u32 frames, float* output) {
    for (u32 slot = 0; slot < 8; ++slot) {
        planar[slot].resize(frames);
        const float* source = input + layout[slot];
        float* target = planar[slot].data();
        for (u32 i = 0; i < frames; ++i) {
            target[i] = source[size_t{i} * 8];
        }
    }
    Render(frames, output);
}

void SurroundVirtualizer::Process(const s16* input, u32 frames, float* output) {
    for (u32 slot = 0; slot < 8; ++slot) {
        planar[slot].resize(frames);
        const s16* source = input + layout[slot];
        float* target = planar[slot].data();
        for (u32 i = 0; i < frames; ++i) {
            target[i] = source[size_t{i} * 8] / 32768.0f;
        }
    }
    Render(frames, output);
}

void SurroundVirtualizer::Render(u32 frames, float* output) {
    std::fill_n(output, size_t{frames} * 2, 0.0f);

    for (u32 speaker = 0; speaker < NumSpeakers; ++speaker) {
        const float angle = SpeakerAngle[speaker] * Pi / 180.0f;
        Audio3d::ObjectPlacement placement;
        placement.x = std::sin(angle);
        placement.z = -std::cos(angle);
        placement.gain = Headroom;
        speakers[speaker].Process(planar[SpeakerSlot[speaker]].data(), frames, output, placement,
                                  sample_rate);
    }

    // The subwoofer channel has no direction; only what is deep enough to be one is kept.
    const float smoothing = 1.0f - std::exp(-2.0f * Pi * LfeCutoff / sample_rate);
    const float* lfe = planar[LfeSlot].data();
    float bass = bass_state;
    for (u32 i = 0; i < frames; ++i) {
        bass += (lfe[i] - bass) * smoothing;
        output[size_t{i} * 2] += bass * LfeLevel;
        output[size_t{i} * 2 + 1] += bass * LfeLevel;
    }
    bass_state = std::isfinite(bass) ? bass : 0.0f;

    // What still exceeds full scale is turned down as a whole rather than clipped.
    float gain = limiter_gain;
    for (u32 i = 0; i < frames; ++i) {
        const float peak =
            std::max(std::abs(output[size_t{i} * 2]), std::abs(output[size_t{i} * 2 + 1]));
        const float target = peak > 1.0f ? 1.0f / peak : 1.0f;
        gain = target < gain ? target : gain + (target - gain) * 0.0002f;
        output[size_t{i} * 2] *= gain;
        output[size_t{i} * 2 + 1] *= gain;
    }
    limiter_gain = std::isfinite(gain) ? gain : 1.0f;
}

} // namespace Libraries::AudioOut
