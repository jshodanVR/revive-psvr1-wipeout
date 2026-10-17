// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <optional>
#include <span>
#include <thread>
#include <vector>

#include "common/logging/log.h"
#include "core/emulator_settings.h"
#include "core/libraries/audio/audioout.h"
#include "core/libraries/audio/audioout_backend.h"
#include "core/libraries/audio/surround_virtualizer.h"
#include "platform/bachata/audio_transport.h"

namespace Libraries::AudioOut {
namespace {

class BachataPortBackend final : public PortBackend {
public:
    explicit BachataPortBackend(const PortOut& port)
        : buffer_frames(port.buffer_frames), sample_rate(port.sample_rate),
          channels(port.format_info.num_channels), is_float(port.format_info.is_float),
          sample_size(port.format_info.sample_size) {
        // The other end is a pair of speakers at the ears: surround sound is rendered for
        // that instead of being folded down. SHADPS4_VIRTUAL_SURROUND=0 turns it off.
        const char* surround = std::getenv("SHADPS4_VIRTUAL_SURROUND");
        if (channels == 8 && (surround == nullptr || surround[0] != '0')) {
            virtualizer.emplace(port.format_info.channel_layout, sample_rate);
        }
    }

    void Output(void* source) override {
        if (!transport) {
            // Titles open ports they never play anything on. A connection (and with it an
            // audio stream on the device) is only made once there is something to hear.
            if (source != nullptr && !failed && !IsSilent(source)) {
                Connect();
            }
            if (!transport) {
                return;
            }
        }
        if (source == nullptr) {
            return;
        }
        const float global_gain = EmulatorSettings.GetVolumeSlider() * 0.01f;
        const float gain =
            std::clamp(channel_gain.load(std::memory_order_relaxed) * global_gain, 0.0f, 1.0f);
        std::vector<std::int16_t> pcm;
        if (virtualizer) {
            stereo.resize(std::size_t{buffer_frames} * 2);
            if (is_float) {
                virtualizer->Process(static_cast<const float*>(source), buffer_frames,
                                     stereo.data());
            } else {
                virtualizer->Process(static_cast<const s16*>(source), buffer_frames, stereo.data());
            }
            pcm = Platform::Bachata::ConvertPcmToStereo(stereo.data(), buffer_frames, 2, true, gain);
        } else {
            pcm = Platform::Bachata::ConvertPcmToStereo(source, buffer_frames, channels, is_float,
                                                        gain);
        }
        const auto bytes = std::as_bytes(std::span{pcm});
        if (!transport->Write({reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size()})) {
            LOG_ERROR(Lib_AudioOut, "Bachata audio transport disconnected");
            transport.reset();
            failed = true;
        }
    }

    bool IsDevicePaced() const override {
        // The host plays what it is sent on a real audio stream and takes no more than that
        // stream has room for.
        return transport.has_value();
    }

    void SetVolume(const std::array<int, 8>& volumes) override {
        int maximum = 0;
        for (std::uint8_t channel = 0; channel < channels; ++channel) {
            maximum = std::max(maximum, std::abs(volumes[channel]));
        }
        channel_gain.store(std::clamp(maximum / 32768.0f, 0.0f, 1.0f),
                           std::memory_order_relaxed);
    }

private:
    bool IsSilent(const void* source) const {
        const auto* bytes = static_cast<const u8*>(source);
        const std::size_t size = std::size_t{buffer_frames} * channels * sample_size;
        // Both integer and floating point silence are all zero bytes.
        return std::all_of(bytes, bytes + size, [](u8 byte) { return byte == 0; });
    }

    void Connect() {
        const char* socket_path = std::getenv("BACHATA_ALSA_SOCKET");
        if (socket_path == nullptr || socket_path[0] != '/') {
            failed = true;
            return;
        }
        transport = Platform::Bachata::AudioTransport::Connect(socket_path);
        const std::uint32_t output_size = buffer_frames * 2 * sizeof(std::int16_t);
        if (!transport || !transport->Prepare(2, Platform::Bachata::AudioSampleType::S16LittleEndian,
                                              sample_rate, output_size)) {
            transport.reset();
            failed = true;
            LOG_ERROR(Lib_AudioOut, "Failed to connect Bachata audio transport");
            return;
        }
        LOG_INFO(Lib_AudioOut,
                 "Opened Bachata audio transport ({} Hz, {} input channels, virtual surround {})",
                 sample_rate, channels, virtualizer.has_value());
    }

    std::optional<Platform::Bachata::AudioTransport> transport;
    std::optional<SurroundVirtualizer> virtualizer;
    std::vector<float> stereo;
    std::uint32_t buffer_frames;
    std::uint32_t sample_rate;
    std::uint8_t channels;
    bool is_float;
    std::uint8_t sample_size;
    bool failed{};
    std::atomic<float> channel_gain{1.0f};
};

} // namespace

std::unique_ptr<PortBackend> BachataAudioOut::Open(PortOut& port) {
    return std::make_unique<BachataPortBackend>(port);
}

} // namespace Libraries::AudioOut
