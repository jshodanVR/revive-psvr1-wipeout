// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <vector>

#include "common/types.h"
#include "core/libraries/ngs2/ngs2.h"

namespace Libraries::Ngs2 {

/// OrbisNgs2WaveformFormat::waveformType, and the sample format of render buffers.
enum WaveformType : u32 {
    WaveformNone = 0x00,
    WaveformPcmI8 = 0x10,
    WaveformPcmU8 = 0x11,
    WaveformPcmI16L = 0x12,
    WaveformPcmI16B = 0x13,
    WaveformPcmI24L = 0x14,
    WaveformPcmI24B = 0x15,
    WaveformPcmI32L = 0x16,
    WaveformPcmI32B = 0x17,
    WaveformPcmF32L = 0x18,
    WaveformPcmF32B = 0x19,
    WaveformPcmF64L = 0x1a,
    WaveformPcmF64B = 0x1b,
    WaveformVag = 0x1c,
    WaveformAtrac9 = 0x40,
};

/// Turns the encoded audio a sampler voice is fed into floating point samples, a frame at a time.
class WaveformDecoder {
public:
    WaveformDecoder() = default;
    ~WaveformDecoder();
    WaveformDecoder(const WaveformDecoder&) = delete;
    WaveformDecoder& operator=(const WaveformDecoder&) = delete;

    /// Returns false when the format is not one that can be decoded.
    bool Setup(const OrbisNgs2WaveformFormat& format);

    /// Forgets what earlier data left behind; the next frame is the first one of a block.
    void Restart();

    /// Decodes the frame at the start of `data` and appends its samples to `out`, interleaved.
    /// Returns the number of bytes used, or 0 when `size` bytes do not hold a whole frame.
    size_t DecodeFrame(const u8* data, size_t size, std::vector<float>& out);

    u32 Channels() const {
        return channels;
    }

private:
    void Release();

    u32 type{WaveformNone};
    u32 channels{};

    // ATRAC9
    void* atrac9{};
    u8 atrac9_config[4]{};
    int superframe_size{};
    int frames_in_superframe{};
    int frame_samples{};
    int frame_index{};
    int superframe_remaining{};

    // VAG: the four samples before the current one, per channel.
    float vag_history[ORBIS_NGS2_MAX_VOICE_CHANNELS][4]{};
};

} // namespace Libraries::Ngs2
