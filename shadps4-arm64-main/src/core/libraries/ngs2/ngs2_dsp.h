// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <vector>

#include "common/types.h"

// Signal processing blocks of the Ngs2 racks. Everything works on one channel of one render
// block at a time ("grain", at most MaxGrain samples) unless it says otherwise.

namespace Libraries::Ngs2::Dsp {

inline constexpr u32 MaxChannels = 8;
inline constexpr u32 MaxGrain = 1024;

/// OrbisNgs2*VoiceFilterParam::type. The numbering follows how titles use them: 1 with a
/// cutoff that rests at 24 kHz and comes down to muffle a sound, and 6, 7 and 8 as the low,
/// middle and high band of a tone control, all three with a gain.
enum FilterType : u32 {
    FilterBypass = 0,
    FilterLowPass = 1,
    FilterHighPass = 2,
    FilterBandPass = 3,
    FilterBandStop = 4,
    FilterAllPass = 5,
    FilterLowShelf = 6,
    FilterBell = 7,
    FilterHighShelf = 8,
};

struct Biquad {
    float b0{1.0f};
    float b1{};
    float b2{};
    float a1{};
    float a2{};
    /// Passes its input through unchanged, so there is nothing to compute.
    bool identity{true};
};

struct BiquadState {
    float z1{};
    float z2{};
};

/// `level` is a linear gain and only matters to the shelving and bell types.
Biquad MakeFilter(u32 type, float fc, float q, float level, float sample_rate);
void RunBiquad(const Biquad& filter, BiquadState& state, float* samples, u32 count);

/// A circular buffer read at a distance behind the write position.
class DelayLine {
public:
    void Resize(u32 max_delay);
    void Clear();
    void Write(float sample) {
        buffer[position] = sample;
        if (++position == buffer.size()) {
            position = 0;
        }
    }
    /// The sample written `delay` writes ago; 1 is the most recent one.
    float Read(u32 delay) const {
        const u32 size = static_cast<u32>(buffer.size());
        return buffer[(position + size - delay) % size];
    }
    /// Reads at a fractional distance, interpolating between neighbours.
    float ReadFractional(float delay) const;
    u32 Capacity() const {
        return static_cast<u32>(buffer.size()) - 1;
    }

private:
    std::vector<float> buffer{0.0f, 0.0f};
    u32 position{};
};

/// The I3DL2 properties a reverb voice is driven with.
struct ReverbParams {
    float wet{1.0f};
    float dry{0.0f};
    float room_mb{-1000.0f};
    float room_hf_mb{-100.0f};
    float decay_time{1.49f};
    float decay_hf_ratio{0.83f};
    float reflections_mb{-2602.0f};
    float reflections_delay{0.007f};
    float reverb_mb{200.0f};
    float reverb_delay{0.011f};
    float diffusion{100.0f};
    float density{100.0f};
    float hf_reference{5000.0f};
};

/// Eight channels in, eight channels out: early reflections off a tapped delay per channel and
/// a feedback delay network, one line per channel, for the tail.
class Reverb {
public:
    void Setup(float sample_rate);
    void SetParams(const ReverbParams& params);
    void Process(float* const channels[MaxChannels], u32 count);

private:
    void Update();

    static constexpr u32 NumEarlyTaps = 4;

    float sample_rate{48000.0f};
    ReverbParams params;
    bool configured{};

    std::array<DelayLine, MaxChannels> predelay;
    std::array<DelayLine, MaxChannels> diffuser_a;
    std::array<DelayLine, MaxChannels> diffuser_b;
    std::array<DelayLine, MaxChannels> lines;
    std::array<u32, MaxChannels> line_length{};
    std::array<float, MaxChannels> line_gain{};
    std::array<float, MaxChannels> line_damping{};
    std::array<float, MaxChannels> line_norm{};
    std::array<float, MaxChannels> damping_state{};
    std::array<float, MaxChannels> input_state{};
    std::array<u32, NumEarlyTaps> early_delay{};
    u32 late_delay{1};
    float early_gain{};
    float late_gain{};
    float input_lowpass{};
    float diffusion_gain{};
};

struct ChorusParams {
    u32 num_phases{1};
    float input_level{1.0f};
    float delay_ms{20.0f};
    float modulation_rate{1.0f};
    float modulation_depth{0.5f};
    float feedback{};
    float wet{};
    float dry{1.0f};
};

class Chorus {
public:
    void Setup(float sample_rate);
    void SetParams(const ChorusParams& params) {
        this->params = params;
    }
    void Process(float* const channels[MaxChannels], u32 num_channels, u32 count);

private:
    float sample_rate{48000.0f};
    ChorusParams params;
    std::array<DelayLine, MaxChannels> lines;
    float phase{};
};

struct DelayParams {
    static constexpr u32 MaxTaps = 8;
    float dry{1.0f};
    float wet{};
    float input_level{1.0f};
    float feedback{};
    float lowpass_fc{20000.0f};
    u32 num_taps{};
    std::array<float, MaxTaps> tap_level{};
    std::array<float, MaxTaps> tap_ms{};
};

class TapDelay {
public:
    void Setup(float sample_rate, float max_ms);
    void SetParams(const DelayParams& params) {
        this->params = params;
    }
    void Process(float* const channels[MaxChannels], u32 num_channels, u32 count);

private:
    float sample_rate{48000.0f};
    DelayParams params;
    std::array<DelayLine, MaxChannels> lines;
    std::array<float, MaxChannels> lowpass_state{};
};

/// Changes pitch without changing length, by reading a short delay at a different speed
/// through two overlapping windows.
class PitchShift {
public:
    void Setup(float sample_rate);
    void SetCents(s32 cents);
    void Process(float* const channels[MaxChannels], u32 num_channels, u32 count);

private:
    float sample_rate{48000.0f};
    float ratio{1.0f};
    float window{};
    float phase{};
    std::array<DelayLine, MaxChannels> lines;
};

/// Keeps peaks under a threshold, the same gain for all channels.
class Limiter {
public:
    void Setup(float sample_rate);
    void Process(float* const channels[MaxChannels], u32 num_channels, u32 count, float threshold);
    float Gain() const {
        return gain;
    }

private:
    float gain{1.0f};
    float release{0.0005f};
};

} // namespace Libraries::Ngs2::Dsp
