// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <cmath>

#include "core/libraries/audio3d/audio3d_spatializer.h"

namespace Libraries::Audio3d {

namespace {

constexpr float Pi = 3.14159265358979323846f;
constexpr float HeadRadius = 0.0875f;   // metres
constexpr float SpeedOfSound = 343.0f;  // metres per second
// The ear on the far side of the head loses the most treble for sound arriving from 150 degrees
// off its own axis, where it drops to a tenth; this is the model's fit to measured heads.
constexpr float ShadowAngle = 150.0f / 180.0f * Pi;
constexpr float ShadowFloor = 0.1f;
// The model's treble gain averaged over all directions. Dividing by it keeps sound that comes
// from everywhere, and the mix as a whole, as bright as it was.
constexpr float DiffuseTreble = 0.844f;
constexpr float RearCutoff = 3000.0f;
constexpr float RearAmount = 0.5f;

} // namespace

void Spatializer::Reset() {
    history.fill(0.0f);
    filters = {};
    rear_state[0] = 0.0f;
    rear_state[1] = 0.0f;
    primed = false;
}

void Spatializer::Process(const float* in, u32 count, float* out, const ObjectPlacement& placement,
                          float sample_rate) {
    while (count > MaxBlock) {
        Process(in, MaxBlock, out, placement, sample_rate);
        in += MaxBlock;
        out += size_t{MaxBlock} * 2;
        count -= MaxBlock;
    }
    if (count == 0) {
        return;
    }

    // Where the sound is, reduced to what the model uses: how far to the side and how far
    // behind. A wide source is treated as partly coming from straight ahead.
    const float length = std::sqrt(placement.x * placement.x + placement.y * placement.y +
                                   placement.z * placement.z);
    const float focus =
        1.0f - std::clamp(std::isfinite(placement.spread) ? placement.spread / (2.0f * Pi) : 0.0f,
                          0.0f, 1.0f);
    float lateral = 0.0f;
    float behind = 0.0f;
    if (std::isfinite(length) && length > 1e-4f) {
        lateral = std::clamp(placement.x / length, -1.0f, 1.0f) * focus;
        behind = std::clamp(placement.z / length, 0.0f, 1.0f) * focus;
    }
    const float gain = std::isfinite(placement.gain) ? placement.gain : 0.0f;

    const float head_samples = HeadRadius / SpeedOfSound * sample_rate;
    std::array<Ear, 2> target;
    for (u32 ear = 0; ear < 2; ++ear) {
        // Angle between the sound's direction and the direction this ear faces.
        const float cosine = ear == 0 ? -lateral : lateral;
        const float angle = std::acos(cosine);
        target[ear].delay = angle < Pi * 0.5f ? head_samples * (1.0f - cosine)
                                              : head_samples * (1.0f + angle - Pi * 0.5f);
        target[ear].treble = ((1.0f + ShadowFloor * 0.5f) +
                              (1.0f - ShadowFloor * 0.5f) * std::cos(angle / ShadowAngle * Pi)) /
                             DiffuseTreble;
        target[ear].gain = gain;
    }
    if (!primed) {
        ears = target;
        rear = behind;
        primed = true;
    }

    std::array<float, History + MaxBlock + 1> buffer;
    std::copy(history.begin(), history.end(), buffer.begin());
    std::copy(in, in + count, buffer.begin() + History);
    buffer[History + count] = 0.0f;

    // The head as a first-order shelf: unity at low frequencies, `treble` at high ones, with the
    // corner where the wavelength is about the size of the head.
    const float corner = 2.0f * SpeedOfSound / HeadRadius;
    const float k = 2.0f * sample_rate;
    const float denominator = 1.0f / (corner + k);
    const float a1 = (corner - k) * denominator;
    const float rear_coefficient = 1.0f - std::exp(-2.0f * Pi * RearCutoff / sample_rate);
    const float inverse = 1.0f / static_cast<float>(count);

    for (u32 ear = 0; ear < 2; ++ear) {
        const Ear from = ears[ear];
        const Ear to = target[ear];
        float previous_input = filters[ear].previous_input;
        float previous_output = filters[ear].previous_output;
        float low = rear_state[ear];
        for (u32 i = 0; i < count; ++i) {
            // Everything glides to its new value over the block, a jump would click.
            const float t = static_cast<float>(i + 1) * inverse;
            const float delay = from.delay + (to.delay - from.delay) * t;
            const float treble = from.treble + (to.treble - from.treble) * t;
            const float level = from.gain + (to.gain - from.gain) * t;
            const float dull = (rear + (behind - rear) * t) * RearAmount;

            const float position = static_cast<float>(History + i) - delay;
            const u32 whole = static_cast<u32>(position);
            const float fraction = position - static_cast<float>(whole);
            const float x = buffer[whole] + (buffer[whole + 1] - buffer[whole]) * fraction;

            const float b0 = (corner + k * treble) * denominator;
            const float b1 = (corner - k * treble) * denominator;
            float y = b0 * x + b1 * previous_input - a1 * previous_output;
            previous_input = x;
            previous_output = y;

            low += (y - low) * rear_coefficient;
            y += (low - y) * dull;

            out[size_t{i} * 2 + ear] += y * level;
        }
        filters[ear].previous_input = previous_input;
        filters[ear].previous_output = std::isfinite(previous_output) ? previous_output : 0.0f;
        rear_state[ear] = std::isfinite(low) ? low : 0.0f;
        ears[ear] = to;
    }
    rear = behind;

    std::copy(buffer.begin() + count, buffer.begin() + count + History, history.begin());
}

} // namespace Libraries::Audio3d
