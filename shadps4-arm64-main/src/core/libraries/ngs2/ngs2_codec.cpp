// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <cstring>

#include "common/logging/log.h"
#include "core/libraries/ngs2/ngs2_codec.h"
#include "core/libraries/ngs2/ngs2_hevag_table.h"

extern "C" {
#include <libatrac9.h>
}

namespace Libraries::Ngs2 {

namespace {

constexpr size_t VagFrameSize = 16;
constexpr size_t VagFrameSamples = 28;

/// Bytes per sample of a PCM type, 0 for anything else.
u32 PcmSampleSize(u32 type) {
    switch (type) {
    case WaveformPcmI8:
    case WaveformPcmU8:
        return 1;
    case WaveformPcmI16L:
    case WaveformPcmI16B:
        return 2;
    case WaveformPcmI24L:
    case WaveformPcmI24B:
        return 3;
    case WaveformPcmI32L:
    case WaveformPcmI32B:
    case WaveformPcmF32L:
    case WaveformPcmF32B:
        return 4;
    case WaveformPcmF64L:
    case WaveformPcmF64B:
        return 8;
    default:
        return 0;
    }
}

float ReadPcm(u32 type, const u8* p) {
    switch (type) {
    case WaveformPcmI8:
        return static_cast<s8>(p[0]) / 128.0f;
    case WaveformPcmU8:
        return (static_cast<int>(p[0]) - 128) / 128.0f;
    case WaveformPcmI16L:
        return static_cast<s16>(p[0] | (p[1] << 8)) / 32768.0f;
    case WaveformPcmI16B:
        return static_cast<s16>(p[1] | (p[0] << 8)) / 32768.0f;
    case WaveformPcmI24L:
        return static_cast<s32>((p[0] << 8) | (p[1] << 16) | (static_cast<u32>(p[2]) << 24)) /
               2147483648.0f;
    case WaveformPcmI24B:
        return static_cast<s32>((p[2] << 8) | (p[1] << 16) | (static_cast<u32>(p[0]) << 24)) /
               2147483648.0f;
    case WaveformPcmI32L:
        return static_cast<s32>(p[0] | (p[1] << 8) | (p[2] << 16) |
                                (static_cast<u32>(p[3]) << 24)) /
               2147483648.0f;
    case WaveformPcmI32B:
        return static_cast<s32>(p[3] | (p[2] << 8) | (p[1] << 16) |
                                (static_cast<u32>(p[0]) << 24)) /
               2147483648.0f;
    case WaveformPcmF32L: {
        float value;
        std::memcpy(&value, p, sizeof(value));
        return value;
    }
    case WaveformPcmF32B: {
        const u8 swapped[4] = {p[3], p[2], p[1], p[0]};
        float value;
        std::memcpy(&value, swapped, sizeof(value));
        return value;
    }
    case WaveformPcmF64L: {
        double value;
        std::memcpy(&value, p, sizeof(value));
        return static_cast<float>(value);
    }
    case WaveformPcmF64B: {
        u8 swapped[8];
        for (int i = 0; i < 8; ++i) {
            swapped[i] = p[7 - i];
        }
        double value;
        std::memcpy(&value, swapped, sizeof(value));
        return static_cast<float>(value);
    }
    default:
        return 0.0f;
    }
}

} // namespace

WaveformDecoder::~WaveformDecoder() {
    Release();
}

void WaveformDecoder::Release() {
    if (atrac9 != nullptr) {
        Atrac9ReleaseHandle(atrac9);
        atrac9 = nullptr;
    }
}

bool WaveformDecoder::Setup(const OrbisNgs2WaveformFormat& format) {
    Release();
    type = WaveformNone;
    channels = 0;
    if (format.numChannels == 0 || format.numChannels > ORBIS_NGS2_MAX_VOICE_CHANNELS) {
        return false;
    }

    if (format.waveformType == WaveformAtrac9) {
        // The configuration travels as the number its four bytes spell.
        atrac9_config[0] = static_cast<u8>(format.configData >> 24);
        atrac9_config[1] = static_cast<u8>(format.configData >> 16);
        atrac9_config[2] = static_cast<u8>(format.configData >> 8);
        atrac9_config[3] = static_cast<u8>(format.configData);
        atrac9 = Atrac9GetHandle();
        Atrac9CodecInfo info{};
        if (atrac9 == nullptr || Atrac9InitDecoder(atrac9, atrac9_config) != 0 ||
            Atrac9GetCodecInfo(atrac9, &info) != 0 || info.framesInSuperframe <= 0 ||
            info.channels <= 0 || info.channels > ORBIS_NGS2_MAX_VOICE_CHANNELS) {
            LOG_ERROR(Lib_Ngs2, "unusable ATRAC9 configuration {:#010x}", format.configData);
            Release();
            return false;
        }
        superframe_size = info.superframeSize;
        frames_in_superframe = info.framesInSuperframe;
        frame_samples = info.frameSamples;
        channels = static_cast<u32>(info.channels);
    } else if (format.waveformType == WaveformVag || PcmSampleSize(format.waveformType) != 0) {
        channels = format.numChannels;
    } else {
        LOG_ERROR(Lib_Ngs2, "unknown waveform type {:#x}", format.waveformType);
        return false;
    }
    type = format.waveformType;
    Restart();
    return true;
}

void WaveformDecoder::Restart() {
    std::memset(vag_history, 0, sizeof(vag_history));
    frame_index = 0;
    superframe_remaining = superframe_size;
    if (type == WaveformAtrac9 && atrac9 != nullptr) {
        // The decoder overlaps each frame with the one before it; starting clean keeps the
        // tail of unrelated audio out of the first frame.
        Atrac9ReleaseHandle(atrac9);
        atrac9 = Atrac9GetHandle();
        if (atrac9 != nullptr) {
            Atrac9InitDecoder(atrac9, atrac9_config);
        }
    }
}

size_t WaveformDecoder::DecodeFrame(const u8* data, size_t size, std::vector<float>& out) {
    if (type == WaveformAtrac9) {
        if (atrac9 == nullptr || size < static_cast<size_t>(superframe_remaining)) {
            return 0;
        }
        const size_t start = out.size();
        out.resize(start + static_cast<size_t>(frame_samples) * channels);
        int used = 0;
        if (Atrac9DecodeF32(atrac9, data, out.data() + start, &used, 0) != 0 || used <= 0 ||
            used > superframe_remaining) {
            // A frame that does not decode is played as silence, the stream stays in step.
            std::fill(out.begin() + static_cast<std::ptrdiff_t>(start), out.end(), 0.0f);
            used = superframe_remaining;
            frame_index = frames_in_superframe - 1;
        }
        size_t consumed = static_cast<size_t>(used);
        superframe_remaining -= used;
        if (++frame_index >= frames_in_superframe) {
            // Whatever is left of the superframe is padding.
            consumed += static_cast<size_t>(superframe_remaining);
            superframe_remaining = superframe_size;
            frame_index = 0;
        }
        return consumed;
    }

    if (type == WaveformVag) {
        // One 16-byte frame per channel in turn.
        if (size < VagFrameSize * channels) {
            return 0;
        }
        const size_t start = out.size();
        out.resize(start + VagFrameSamples * channels);
        for (u32 channel = 0; channel < channels; ++channel) {
            const u8* frame = data + VagFrameSize * channel;
            u32 predictor = ((frame[0] >> 4) & 0xf) | (frame[1] & 0xf0);
            const u32 shift = frame[0] & 0xf;
            const u32 flag = frame[1] & 0xf;
            if (predictor > 127) {
                predictor = 0;
            }
            const float* coefficients = HevagCoefficients[predictor];
            float* history = vag_history[channel];
            for (size_t i = 0; i < VagFrameSamples; ++i) {
                float sample = 0.0f;
                // Flag 7 marks a frame that is there to be skipped.
                if (flag < 7) {
                    const u8 byte = frame[2 + i / 2];
                    const int nibble = (i & 1) != 0 ? byte >> 4 : byte & 0xf;
                    const int code = ((nibble ^ 8) - 8) * 4096;
                    sample = static_cast<float>(shift < 32 ? code >> shift : 0) +
                             history[0] * coefficients[0] + history[1] * coefficients[1] +
                             history[2] * coefficients[2] + history[3] * coefficients[3];
                }
                history[3] = history[2];
                history[2] = history[1];
                history[1] = history[0];
                history[0] = sample;
                out[start + i * channels + channel] =
                    std::clamp(sample, -32768.0f, 32767.0f) / 32768.0f;
            }
        }
        return VagFrameSize * channels;
    }

    const u32 sample_size = PcmSampleSize(type);
    if (sample_size == 0) {
        return 0;
    }
    const size_t frame_size = size_t{sample_size} * channels;
    // PCM has no frames of its own; a few hundred samples at a time keeps the queue short.
    const size_t frames = std::min<size_t>(size / frame_size, 256);
    if (frames == 0) {
        return 0;
    }
    const size_t start = out.size();
    out.resize(start + frames * channels);
    for (size_t i = 0; i < frames * channels; ++i) {
        out[start + i] = ReadPcm(type, data + i * sample_size);
    }
    return frames * frame_size;
}

} // namespace Libraries::Ngs2
