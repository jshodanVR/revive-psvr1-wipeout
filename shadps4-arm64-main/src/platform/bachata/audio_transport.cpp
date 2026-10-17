// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "platform/bachata/audio_transport.h"

#include <array>
#include <cerrno>
#include <cstring>
#include <algorithm>
#include <cmath>

#ifndef _WIN32
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#endif

namespace Platform::Bachata {
namespace {

void StoreLittleEndian(std::uint8_t* target, std::uint32_t value) {
    target[0] = static_cast<std::uint8_t>(value);
    target[1] = static_cast<std::uint8_t>(value >> 8);
    target[2] = static_cast<std::uint8_t>(value >> 16);
    target[3] = static_cast<std::uint8_t>(value >> 24);
}

} // namespace

std::vector<std::int16_t> ConvertPcmToStereo(const void* source, std::uint32_t frames,
                                             std::uint8_t channels, bool is_float, float gain) {
    if (source == nullptr || frames == 0 || (channels != 1 && channels != 2 && channels != 8)) {
        return {};
    }
    const float clamped_gain = std::clamp(gain, 0.0f, 1.0f);
    std::vector<std::int16_t> result(static_cast<std::size_t>(frames) * 2);
    const auto convert = [clamped_gain](float sample) {
        const float scaled = std::clamp(sample * clamped_gain, -1.0f, 1.0f);
        if (scaled <= -1.0f) {
            return std::int16_t{-32768};
        }
        return static_cast<std::int16_t>(std::lround(scaled * 32767.0f));
    };
    for (std::uint32_t frame = 0; frame < frames; ++frame) {
        float left = 0.0f;
        float right = 0.0f;
        float in[8]{};
        if (is_float) {
            const auto* samples = static_cast<const float*>(source) + frame * channels;
            std::copy_n(samples, channels, in);
        } else {
            const auto* samples = static_cast<const std::int16_t*>(source) + frame * channels;
            for (std::uint8_t channel = 0; channel < channels; ++channel) {
                in[channel] = samples[channel] / 32768.0f;
            }
        }
        if (channels == 8) {
            // Front pair, centre, LFE, then two surround pairs: everything on the left goes to
            // the left, the centre and the LFE to both.
            constexpr float Half = 0.70710678f;
            const float shared = in[2] * Half + in[3] * 0.5f;
            left = (in[0] + shared + (in[4] + in[6]) * Half) * Half;
            right = (in[1] + shared + (in[5] + in[7]) * Half) * Half;
        } else {
            left = in[0];
            right = channels == 1 ? left : in[1];
        }
        result[frame * 2] = convert(left);
        result[frame * 2 + 1] = convert(right);
    }
    return result;
}

AudioTransport::AudioTransport(int fd) : fd_(fd) {}

AudioTransport::AudioTransport(AudioTransport&& other) noexcept : fd_(other.fd_) {
    other.fd_ = -1;
}

AudioTransport& AudioTransport::operator=(AudioTransport&& other) noexcept {
    if (this != &other) {
        Close();
        fd_ = other.fd_;
        other.fd_ = -1;
    }
    return *this;
}

AudioTransport::~AudioTransport() {
    Close();
}

std::optional<AudioTransport> AudioTransport::Connect(const std::filesystem::path& socket_path) {
#ifdef _WIN32
    (void)socket_path;
    return std::nullopt;
#else
    const auto native = socket_path.string();
    if (!socket_path.is_absolute() || native.size() >= sizeof(sockaddr_un::sun_path)) {
        return std::nullopt;
    }
    const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        return std::nullopt;
    }
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    std::strncpy(address.sun_path, native.c_str(), sizeof(address.sun_path) - 1);
    if (::connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
        ::close(fd);
        return std::nullopt;
    }
    // Sound waiting in the socket is sound heard late. A small buffer makes a write block as
    // soon as the other end falls behind, which then paces the guest by the real audio device
    // instead of letting a backlog build up. (The kernel doubles the figure and takes it to
    // its minimum, which holds two of a game's buffers: a hundredth of a second.)
    const int send_buffer = 2048;
    ::setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &send_buffer, sizeof(send_buffer));
    return AudioTransport(fd);
#endif
}

bool AudioTransport::Prepare(std::uint8_t channels, AudioSampleType type,
                             std::uint32_t sample_rate, std::uint32_t buffer_size) {
    std::array<std::uint8_t, 10> payload{channels, static_cast<std::uint8_t>(type)};
    StoreLittleEndian(payload.data() + 2, sample_rate);
    StoreLittleEndian(payload.data() + 6, buffer_size);
    return SendRequest(4, payload);
}

bool AudioTransport::Write(std::span<const std::uint8_t> pcm) {
    return !pcm.empty() && SendRequest(5, pcm);
}

bool AudioTransport::Capture(std::uint32_t sample_rate) {
    std::array<std::uint8_t, 10> payload{2, static_cast<std::uint8_t>(AudioSampleType::S16LittleEndian)};
    StoreLittleEndian(payload.data() + 2, sample_rate);
    return SendRequest(6, payload);
}

std::ptrdiff_t AudioTransport::Read(std::span<std::uint8_t> pcm) {
#ifdef _WIN32
    (void)pcm;
    return -1;
#else
    if (!IsConnected()) {
        return -1;
    }
    while (true) {
        const ssize_t count = ::recv(fd_, pcm.data(), pcm.size(), MSG_DONTWAIT);
        if (count > 0) {
            return count;
        }
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            return 0;
        }
        return -1;
    }
#endif
}

bool AudioTransport::IsConnected() const {
    return fd_ >= 0;
}

bool AudioTransport::SendRequest(std::uint8_t code, std::span<const std::uint8_t> payload) {
    if (!IsConnected() || payload.size() > UINT32_MAX) {
        return false;
    }
#ifdef _WIN32
    (void)code;
    return false;
#else
    // Header and payload in one piece: the socket's small buffer is counted in pieces, each
    // with an overhead larger than a header.
    message_.resize(5 + payload.size());
    message_[0] = code;
    StoreLittleEndian(message_.data() + 1, static_cast<std::uint32_t>(payload.size()));
    std::copy(payload.begin(), payload.end(), message_.begin() + 5);
    std::span<const std::uint8_t> rest{message_};
    while (!rest.empty()) {
        const ssize_t written = ::send(fd_, rest.data(), rest.size(), MSG_NOSIGNAL);
        if (written < 0 && errno == EINTR) {
            continue;
        }
        if (written <= 0) {
            return false;
        }
        rest = rest.subspan(static_cast<std::size_t>(written));
    }
    return true;
#endif
}

void AudioTransport::Close() {
#ifndef _WIN32
    if (fd_ >= 0) {
        SendRequest(0, {});
        ::close(fd_);
        fd_ = -1;
    }
#endif
}

} // namespace Platform::Bachata
