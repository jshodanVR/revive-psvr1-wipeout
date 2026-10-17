// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <vector>

namespace Platform::Bachata {

enum class AudioSampleType : std::uint8_t {
    U8 = 0,
    S16LittleEndian = 1,
    S16BigEndian = 2,
    FloatLittleEndian = 3,
    FloatBigEndian = 4,
};

std::vector<std::int16_t> ConvertPcmToStereo(const void* source, std::uint32_t frames,
                                             std::uint8_t channels, bool is_float, float gain);

class AudioTransport {
public:
    explicit AudioTransport(int fd);
    AudioTransport(const AudioTransport&) = delete;
    AudioTransport& operator=(const AudioTransport&) = delete;
    AudioTransport(AudioTransport&& other) noexcept;
    AudioTransport& operator=(AudioTransport&& other) noexcept;
    ~AudioTransport();

    static std::optional<AudioTransport> Connect(const std::filesystem::path& socket_path);

    bool Prepare(std::uint8_t channels, AudioSampleType type, std::uint32_t sample_rate,
                 std::uint32_t buffer_size);
    bool Write(std::span<const std::uint8_t> pcm);
    /// Turns the connection around: instead of taking sound, the other end sends what its
    /// microphone hears, as a plain stream of 16-bit stereo frames at `sample_rate`.
    bool Capture(std::uint32_t sample_rate);
    /// Takes what has arrived, without waiting for more. The number of bytes put into `pcm`,
    /// or -1 once the other end is gone.
    std::ptrdiff_t Read(std::span<std::uint8_t> pcm);
    [[nodiscard]] bool IsConnected() const;

private:
    bool SendRequest(std::uint8_t code, std::span<const std::uint8_t> payload);
    void Close();

    int fd_ = -1;
    /// The request being sent, kept between calls for its memory.
    std::vector<std::uint8_t> message_;
};

} // namespace Platform::Bachata
