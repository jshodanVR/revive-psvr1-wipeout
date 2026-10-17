// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <cmath>

#include "core/libraries/ngs2/ngs2_dsp.h"

namespace Libraries::Ngs2::Dsp {

namespace {

constexpr float Pi = 3.14159265358979323846f;
// Keeps recirculating signals from decaying into denormal numbers, which are slow to compute.
constexpr float AntiDenormal = 1e-18f;
// Front left, front right, centre, LFE, then the surrounds.
constexpr u32 LfeChannel = 3;

float FromMillibel(float millibel) {
    return std::pow(10.0f, std::clamp(millibel, -10000.0f, 2000.0f) / 2000.0f);
}

/// Coefficient of the one-pole low-pass y = (1 - d) x + d y' whose gain at `frequency` is
/// `gain` (below 1).
float OnePoleForGain(float gain, float frequency, float sample_rate) {
    const float r2 = gain * gain;
    if (r2 >= 0.9999f) {
        return 0.0f;
    }
    const float c = std::cos(2.0f * Pi * std::min(frequency, sample_rate * 0.45f) / sample_rate);
    const float a = r2 - 1.0f;
    const float b = 2.0f - 2.0f * r2 * c;
    const float discriminant = std::max(b * b - 4.0f * a * a, 0.0f);
    return std::clamp((-b + std::sqrt(discriminant)) / (2.0f * a), 0.0f, 0.98f);
}

} // namespace

Biquad MakeFilter(u32 type, float fc, float q, float level, float sample_rate) {
    Biquad filter;
    const bool uses_level = type == FilterLowShelf || type == FilterHighShelf || type == FilterBell;
    if (type == FilterBypass || type > FilterHighShelf || !std::isfinite(fc) ||
        !std::isfinite(q) || !std::isfinite(level) ||
        (uses_level && std::abs(level - 1.0f) < 1e-3f) ||
        // A low-pass at or above half the sample rate lets everything through.
        (type == FilterLowPass && fc >= sample_rate * 0.49f)) {
        return filter;
    }
    fc = std::clamp(fc, 10.0f, sample_rate * 0.49f);
    q = std::clamp(q, 0.05f, 50.0f);
    const float w0 = 2.0f * Pi * fc / sample_rate;
    const float cs = std::cos(w0);
    const float alpha = std::sin(w0) / (2.0f * q);
    // Shelves and bells take the square root of the gain they are asked for.
    const float a = std::sqrt(std::clamp(level, 0.001f, 1000.0f));
    const float beta = 2.0f * std::sqrt(a) * alpha;

    float b0 = 1.0f, b1 = 0.0f, b2 = 0.0f, a0 = 1.0f, a1 = 0.0f, a2 = 0.0f;
    switch (type) {
    case FilterLowPass:
        b0 = (1.0f - cs) * 0.5f;
        b1 = 1.0f - cs;
        b2 = b0;
        a0 = 1.0f + alpha;
        a1 = -2.0f * cs;
        a2 = 1.0f - alpha;
        break;
    case FilterHighPass:
        b0 = (1.0f + cs) * 0.5f;
        b1 = -(1.0f + cs);
        b2 = b0;
        a0 = 1.0f + alpha;
        a1 = -2.0f * cs;
        a2 = 1.0f - alpha;
        break;
    case FilterBandPass:
        b0 = alpha;
        b1 = 0.0f;
        b2 = -alpha;
        a0 = 1.0f + alpha;
        a1 = -2.0f * cs;
        a2 = 1.0f - alpha;
        break;
    case FilterBandStop:
        b0 = 1.0f;
        b1 = -2.0f * cs;
        b2 = 1.0f;
        a0 = 1.0f + alpha;
        a1 = -2.0f * cs;
        a2 = 1.0f - alpha;
        break;
    case FilterAllPass:
        b0 = 1.0f - alpha;
        b1 = -2.0f * cs;
        b2 = 1.0f + alpha;
        a0 = 1.0f + alpha;
        a1 = -2.0f * cs;
        a2 = 1.0f - alpha;
        break;
    case FilterLowShelf:
        b0 = a * ((a + 1.0f) - (a - 1.0f) * cs + beta);
        b1 = 2.0f * a * ((a - 1.0f) - (a + 1.0f) * cs);
        b2 = a * ((a + 1.0f) - (a - 1.0f) * cs - beta);
        a0 = (a + 1.0f) + (a - 1.0f) * cs + beta;
        a1 = -2.0f * ((a - 1.0f) + (a + 1.0f) * cs);
        a2 = (a + 1.0f) + (a - 1.0f) * cs - beta;
        break;
    case FilterHighShelf:
        b0 = a * ((a + 1.0f) + (a - 1.0f) * cs + beta);
        b1 = -2.0f * a * ((a - 1.0f) + (a + 1.0f) * cs);
        b2 = a * ((a + 1.0f) + (a - 1.0f) * cs - beta);
        a0 = (a + 1.0f) - (a - 1.0f) * cs + beta;
        a1 = 2.0f * ((a - 1.0f) - (a + 1.0f) * cs);
        a2 = (a + 1.0f) - (a - 1.0f) * cs - beta;
        break;
    case FilterBell:
        b0 = 1.0f + alpha * a;
        b1 = -2.0f * cs;
        b2 = 1.0f - alpha * a;
        a0 = 1.0f + alpha / a;
        a1 = -2.0f * cs;
        a2 = 1.0f - alpha / a;
        break;
    default:
        return filter;
    }
    filter.b0 = b0 / a0;
    filter.b1 = b1 / a0;
    filter.b2 = b2 / a0;
    filter.a1 = a1 / a0;
    filter.a2 = a2 / a0;
    filter.identity = false;
    return filter;
}

void RunBiquad(const Biquad& filter, BiquadState& state, float* samples, u32 count) {
    float z1 = state.z1;
    float z2 = state.z2;
    for (u32 i = 0; i < count; ++i) {
        const float x = samples[i];
        const float y = filter.b0 * x + z1;
        z1 = filter.b1 * x - filter.a1 * y + z2;
        z2 = filter.b2 * x - filter.a2 * y;
        samples[i] = y;
    }
    // A filter that blows up must not poison the voice for good.
    if (!std::isfinite(z1) || !std::isfinite(z2)) {
        z1 = 0.0f;
        z2 = 0.0f;
    }
    state.z1 = std::abs(z1) < 1e-20f ? 0.0f : z1;
    state.z2 = std::abs(z2) < 1e-20f ? 0.0f : z2;
}

void DelayLine::Resize(u32 max_delay) {
    buffer.assign(std::max<u32>(max_delay, 1) + 1, 0.0f);
    position = 0;
}

void DelayLine::Clear() {
    std::fill(buffer.begin(), buffer.end(), 0.0f);
}

float DelayLine::ReadFractional(float delay) const {
    const float limit = static_cast<float>(Capacity() - 1);
    delay = std::clamp(delay, 1.0f, limit);
    const u32 whole = static_cast<u32>(delay);
    const float fraction = delay - static_cast<float>(whole);
    return Read(whole) * (1.0f - fraction) + Read(whole + 1) * fraction;
}

// --- reverb -------------------------------------------------------------------------------------

void Reverb::Setup(float sample_rate_) {
    sample_rate = sample_rate_;
    const float scale = sample_rate / 48000.0f;
    // Mutually prime lengths between 30 and 54 ms keep the lines from reinforcing each other.
    static constexpr std::array<u32, MaxChannels> Lengths = {1427, 1637, 1783, 1973,
                                                             2099, 2213, 2417, 2591};
    static constexpr std::array<u32, MaxChannels> DiffuserA = {211, 223, 227, 229,
                                                               233, 239, 241, 251};
    static constexpr std::array<u32, MaxChannels> DiffuserB = {347, 349, 353, 359,
                                                               367, 373, 379, 383};
    for (u32 c = 0; c < MaxChannels; ++c) {
        line_length[c] = std::max<u32>(static_cast<u32>(static_cast<float>(Lengths[c]) * scale), 8);
        lines[c].Resize(line_length[c] + 1);
        diffuser_a[c].Resize(static_cast<u32>(static_cast<float>(DiffuserA[c]) * scale) + 1);
        diffuser_b[c].Resize(static_cast<u32>(static_cast<float>(DiffuserB[c]) * scale) + 1);
        // Reflections and the start of the tail can be up to 0.3 s + 0.1 s late.
        predelay[c].Resize(static_cast<u32>(sample_rate * 1.2f));
    }
    damping_state.fill(0.0f);
    input_state.fill(0.0f);
    configured = true;
    Update();
}

void Reverb::SetParams(const ReverbParams& new_params) {
    params = new_params;
    if (configured) {
        Update();
    }
}

void Reverb::Update() {
    const float decay = std::clamp(params.decay_time, 0.1f, 20.0f);
    const float hf_ratio = std::clamp(params.decay_hf_ratio, 0.1f, 2.0f);
    for (u32 c = 0; c < MaxChannels; ++c) {
        const float length = static_cast<float>(line_length[c]);
        const float gain = std::pow(10.0f, -3.0f * length / (decay * sample_rate));
        line_gain[c] = gain;
        line_norm[c] = std::sqrt(std::max(1.0f - gain * gain, 0.0f));
        if (hf_ratio < 1.0f) {
            const float hf_gain = std::pow(10.0f, -3.0f * length / (decay * hf_ratio * sample_rate));
            line_damping[c] = OnePoleForGain(hf_gain / gain, params.hf_reference, sample_rate);
        } else {
            line_damping[c] = 0.0f;
        }
    }

    const float reflections = std::clamp(params.reflections_delay, 0.0f, 0.3f);
    const float late = std::clamp(params.reverb_delay, 0.0f, 0.1f);
    static constexpr std::array<float, NumEarlyTaps> Spread = {1.0f, 1.31f, 1.73f, 2.19f};
    for (u32 i = 0; i < NumEarlyTaps; ++i) {
        early_delay[i] = std::clamp<u32>(
            static_cast<u32>((reflections * Spread[i] + 0.0012f * static_cast<float>(i)) *
                             sample_rate),
            1, predelay[0].Capacity());
    }
    late_delay = std::clamp<u32>(static_cast<u32>((reflections + late) * sample_rate), 1,
                                 predelay[0].Capacity());

    early_gain = FromMillibel(params.room_mb + params.reflections_mb) * 0.35f;
    late_gain = FromMillibel(params.room_mb + params.reverb_mb) * 0.5f;
    input_lowpass =
        OnePoleForGain(FromMillibel(params.room_hf_mb), params.hf_reference, sample_rate);
    diffusion_gain = 0.6f * std::clamp(params.diffusion, 0.0f, 100.0f) / 100.0f;
}

void Reverb::Process(float* const channels[MaxChannels], u32 count) {
    if (!configured) {
        return;
    }
    static constexpr std::array<float, NumEarlyTaps> EarlyLevel = {0.8f, -0.6f, 0.45f, -0.3f};

    for (u32 i = 0; i < count; ++i) {
        std::array<float, MaxChannels> injected;
        std::array<float, MaxChannels> early;
        std::array<float, MaxChannels> feedback;
        std::array<float, MaxChannels> tail;
        float feedback_sum = 0.0f;

        for (u32 c = 0; c < MaxChannels; ++c) {
            // A room does not echo from the subwoofer: that channel neither feeds the reverb
            // nor carries any of it.
            const float input = c == LfeChannel ? 0.0f : channels[c][i];
            input_state[c] = input + (input_state[c] - input) * input_lowpass;
            predelay[c].Write(input_state[c] + AntiDenormal);

            float reflections = 0.0f;
            for (u32 tap = 0; tap < NumEarlyTaps; ++tap) {
                reflections += predelay[c].Read(early_delay[tap]) * EarlyLevel[tap];
            }
            early[c] = reflections;

            // Two all-pass stages smear the input before it enters the tail.
            float x = predelay[c].Read(late_delay);
            float delayed = diffuser_a[c].Read(diffuser_a[c].Capacity());
            float y = delayed - diffusion_gain * x;
            diffuser_a[c].Write(x + diffusion_gain * y);
            x = y;
            delayed = diffuser_b[c].Read(diffuser_b[c].Capacity());
            y = delayed - diffusion_gain * x;
            diffuser_b[c].Write(x + diffusion_gain * y);
            injected[c] = y;

            tail[c] = lines[c].Read(line_length[c]);
            damping_state[c] = tail[c] + (damping_state[c] - tail[c]) * line_damping[c];
            feedback[c] = damping_state[c] * line_gain[c];
            feedback_sum += feedback[c];
        }

        // What comes out of every line goes back into all of them (a Householder reflection),
        // which is what makes the echoes dense.
        const float spread = feedback_sum * (2.0f / MaxChannels);
        for (u32 c = 0; c < MaxChannels; ++c) {
            lines[c].Write(injected[c] + feedback[c] - spread);
            if (c == LfeChannel) {
                channels[c][i] *= params.dry;
                continue;
            }
            const float wet = early[c] * early_gain + tail[c] * line_norm[c] * late_gain;
            channels[c][i] = channels[c][i] * params.dry + wet * params.wet;
        }
    }

    for (u32 c = 0; c < MaxChannels; ++c) {
        if (!std::isfinite(damping_state[c]) || !std::isfinite(input_state[c])) {
            damping_state[c] = 0.0f;
            input_state[c] = 0.0f;
            lines[c].Clear();
            predelay[c].Clear();
            diffuser_a[c].Clear();
            diffuser_b[c].Clear();
        }
    }
}

// --- chorus -------------------------------------------------------------------------------------

void Chorus::Setup(float sample_rate_) {
    sample_rate = sample_rate_;
    for (auto& line : lines) {
        // Room for the longest delay with its full modulation swing.
        line.Resize(static_cast<u32>(sample_rate * 0.25f));
    }
}

void Chorus::Process(float* const channels[MaxChannels], u32 num_channels, u32 count) {
    const u32 phases = std::clamp<u32>(params.num_phases, 1, 4);
    const float limit = static_cast<float>(lines[0].Capacity() - 2);
    const float base = std::clamp(params.delay_ms * 0.001f * sample_rate, 2.0f, limit * 0.5f);
    const float depth = base * std::clamp(params.modulation_depth, 0.0f, 0.95f);
    const float step = 2.0f * Pi * std::clamp(params.modulation_rate, 0.0f, 50.0f) / sample_rate;
    const float feedback = std::clamp(params.feedback, -0.95f, 0.95f);
    const bool audible = params.wet != 0.0f;

    for (u32 i = 0; i < count; ++i) {
        for (u32 c = 0; c < num_channels; ++c) {
            const float input = channels[c][i];
            float wet = 0.0f;
            if (audible) {
                for (u32 p = 0; p < phases; ++p) {
                    // Neighbouring channels are a quarter turn apart, which widens the effect.
                    const float angle = phase + 2.0f * Pi * static_cast<float>(p) /
                                                    static_cast<float>(phases) +
                                        0.5f * Pi * static_cast<float>(c & 1);
                    wet += lines[c].ReadFractional(base + depth * std::sin(angle));
                }
                wet /= static_cast<float>(phases);
            }
            lines[c].Write(input * params.input_level + wet * feedback + AntiDenormal);
            channels[c][i] = input * params.dry + wet * params.wet;
        }
        phase += step;
        if (phase > 2.0f * Pi) {
            phase -= 2.0f * Pi;
        }
    }
}

// --- delay --------------------------------------------------------------------------------------

void TapDelay::Setup(float sample_rate_, float max_ms) {
    sample_rate = sample_rate_;
    const float length = std::clamp(max_ms, 1.0f, 5000.0f) * 0.001f * sample_rate;
    for (auto& line : lines) {
        line.Resize(static_cast<u32>(length) + 2);
    }
    lowpass_state.fill(0.0f);
}

void TapDelay::Process(float* const channels[MaxChannels], u32 num_channels, u32 count) {
    const u32 taps = std::min<u32>(params.num_taps, DelayParams::MaxTaps);
    std::array<u32, DelayParams::MaxTaps> delay{};
    for (u32 t = 0; t < taps; ++t) {
        const float samples = std::isfinite(params.tap_ms[t])
                                  ? params.tap_ms[t] * 0.001f * sample_rate
                                  : 1.0f;
        delay[t] = static_cast<u32>(
            std::clamp(samples, 1.0f, static_cast<float>(lines[0].Capacity())));
    }
    const float fc = std::clamp(params.lowpass_fc, 20.0f, sample_rate * 0.45f);
    const float smoothing = std::exp(-2.0f * Pi * fc / sample_rate);
    const float feedback = std::clamp(params.feedback, -0.95f, 0.95f);
    const bool audible = params.wet != 0.0f && taps != 0;

    for (u32 c = 0; c < num_channels; ++c) {
        float state = lowpass_state[c];
        for (u32 i = 0; i < count; ++i) {
            const float input = channels[c][i];
            float wet = 0.0f;
            float recirculated = 0.0f;
            if (audible) {
                for (u32 t = 0; t < taps; ++t) {
                    wet += lines[c].Read(delay[t]) * params.tap_level[t];
                }
                // Every pass around the loop loses some treble, like a tape echo.
                state = wet + (state - wet) * smoothing;
                recirculated = state * feedback;
            }
            lines[c].Write(input * params.input_level + recirculated + AntiDenormal);
            channels[c][i] = input * params.dry + wet * params.wet;
        }
        lowpass_state[c] = std::isfinite(state) ? state : 0.0f;
    }
}

// --- pitch shift --------------------------------------------------------------------------------

void PitchShift::Setup(float sample_rate_) {
    sample_rate = sample_rate_;
    window = std::floor(sample_rate * 0.045f);
    for (auto& line : lines) {
        line.Resize(static_cast<u32>(window) + 8);
    }
}

void PitchShift::SetCents(s32 cents) {
    ratio = std::pow(2.0f, static_cast<float>(std::clamp(cents, -2400, 2400)) / 1200.0f);
}

void PitchShift::Process(float* const channels[MaxChannels], u32 num_channels, u32 count) {
    if (window < 16.0f) {
        return;
    }
    const bool shifting = std::abs(ratio - 1.0f) > 1e-4f;
    // A read position that falls behind (or catches up) by this much every sample plays the
    // delayed sound at the new pitch; two of them half a window apart hide each other's jumps.
    const float step = (1.0f - ratio) / window;
    float local_phase = phase;
    for (u32 i = 0; i < count; ++i) {
        const float phase_b = local_phase < 0.5f ? local_phase + 0.5f : local_phase - 0.5f;
        const float gain_a = 0.5f - 0.5f * std::cos(2.0f * Pi * local_phase);
        const float gain_b = 1.0f - gain_a;
        for (u32 c = 0; c < num_channels; ++c) {
            lines[c].Write(channels[c][i]);
            if (shifting) {
                channels[c][i] = lines[c].ReadFractional(1.0f + local_phase * window) * gain_a +
                                 lines[c].ReadFractional(1.0f + phase_b * window) * gain_b;
            }
        }
        local_phase += step;
        local_phase -= std::floor(local_phase);
    }
    phase = local_phase;
}

// --- limiter ------------------------------------------------------------------------------------

void Limiter::Setup(float sample_rate) {
    // Gain comes back over about a tenth of a second.
    release = 1.0f - std::exp(-1.0f / (0.1f * sample_rate));
    gain = 1.0f;
}

void Limiter::Process(float* const channels[MaxChannels], u32 num_channels, u32 count,
                      float threshold) {
    threshold = std::max(threshold, 0.01f);
    float current = gain;
    for (u32 i = 0; i < count; ++i) {
        float peak = 0.0f;
        for (u32 c = 0; c < num_channels; ++c) {
            peak = std::max(peak, std::abs(channels[c][i]));
        }
        const float target = peak > threshold ? threshold / peak : 1.0f;
        current = target < current ? target : current + (target - current) * release;
        if (current < 0.9999f) {
            for (u32 c = 0; c < num_channels; ++c) {
                channels[c][i] *= current;
            }
        }
    }
    gain = std::isfinite(current) ? current : 1.0f;
}

} // namespace Libraries::Ngs2::Dsp
