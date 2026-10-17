// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "common/types.h"

namespace Common {

/// Records audio to a 16-bit WAV file so that it can be inspected afterwards. A debugging aid:
/// the emulator is normally killed rather than closed, so the header is kept up to date while
/// writing instead of being fixed up at the end.
class WaveDump {
public:
    WaveDump() = default;
    WaveDump(const WaveDump&) = delete;
    WaveDump& operator=(const WaveDump&) = delete;
    WaveDump(WaveDump&& other) noexcept {
        *this = std::move(other);
    }
    WaveDump& operator=(WaveDump&& other) noexcept {
        std::swap(file, other.file);
        std::swap(channels, other.channels);
        std::swap(sample_rate, other.sample_rate);
        std::swap(frames, other.frames);
        std::swap(limit, other.limit);
        std::swap(since_update, other.since_update);
        return *this;
    }
    ~WaveDump() {
        if (file != nullptr) {
            UpdateHeader();
            std::fclose(file);
        }
    }

    /// `max_frames` bounds the size of the file; later writes are dropped.
    bool Open(const std::string& path, u32 channel_count, u32 rate, u64 max_frames) {
        file = std::fopen(path.c_str(), "wb");
        if (file == nullptr) {
            return false;
        }
        channels = std::max<u32>(channel_count, 1);
        sample_rate = rate;
        limit = max_frames;
        u8 header[44]{};
        const auto put32 = [&](size_t at, u32 value) { std::memcpy(header + at, &value, 4); };
        const auto put16 = [&](size_t at, u16 value) { std::memcpy(header + at, &value, 2); };
        std::memcpy(header, "RIFF", 4);
        std::memcpy(header + 8, "WAVEfmt ", 8);
        put32(16, 16);
        put16(20, 1);
        put16(22, static_cast<u16>(channels));
        put32(24, sample_rate);
        put32(28, sample_rate * channels * 2);
        put16(32, static_cast<u16>(channels * 2));
        put16(34, 16);
        std::memcpy(header + 36, "data", 4);
        std::fwrite(header, 1, sizeof(header), file);
        return true;
    }

    bool IsOpen() const {
        return file != nullptr;
    }

    u32 Channels() const {
        return channels;
    }

    /// Interleaved 16-bit samples.
    void Write(const s16* samples, u32 count) {
        if (file == nullptr || frames >= limit) {
            return;
        }
        std::fwrite(samples, sizeof(s16), size_t{count} * channels, file);
        Advance(count);
    }

    /// Interleaved floating point samples; full scale is 1.
    void Write(const float* samples, u32 count) {
        if (file == nullptr || frames >= limit) {
            return;
        }
        scratch.resize(size_t{count} * channels);
        for (size_t i = 0; i < scratch.size(); ++i) {
            scratch[i] = static_cast<s16>(std::clamp(samples[i], -1.0f, 1.0f) * 32767.0f);
        }
        std::fwrite(scratch.data(), sizeof(s16), scratch.size(), file);
        Advance(count);
    }

    /// One array of floating point samples per channel.
    void WritePlanar(const float* const planar[], u32 count) {
        if (file == nullptr || frames >= limit) {
            return;
        }
        scratch.resize(size_t{count} * channels);
        for (u32 c = 0; c < channels; ++c) {
            for (u32 i = 0; i < count; ++i) {
                scratch[size_t{i} * channels + c] =
                    static_cast<s16>(std::clamp(planar[c][i], -1.0f, 1.0f) * 32767.0f);
            }
        }
        std::fwrite(scratch.data(), sizeof(s16), scratch.size(), file);
        Advance(count);
    }

private:
    void Advance(u32 count) {
        frames += count;
        since_update += count;
        // Twice a second, and once more when the file is full.
        if (since_update >= sample_rate / 2 || frames >= limit) {
            since_update = 0;
            UpdateHeader();
        }
    }

    void UpdateHeader() {
        const u32 data_size = static_cast<u32>(frames * channels * 2);
        const u32 riff_size = data_size + 36;
        std::fseek(file, 4, SEEK_SET);
        std::fwrite(&riff_size, 4, 1, file);
        std::fseek(file, 40, SEEK_SET);
        std::fwrite(&data_size, 4, 1, file);
        std::fseek(file, 0, SEEK_END);
        std::fflush(file);
    }

    std::FILE* file{};
    u32 channels{1};
    u32 sample_rate{48000};
    u64 frames{};
    u64 limit{};
    u32 since_update{};
    std::vector<s16> scratch;
};

} // namespace Common
