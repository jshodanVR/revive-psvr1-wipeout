// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <thread>
#include <vector>

#include "common/logging/log.h"
#include "core/libraries/audio/audioin.h"
#include "core/libraries/audio/audioin_backend.h"
#include "platform/bachata/audio_transport.h"

namespace Libraries::AudioIn {
namespace {

/// A microphone port fed by the host app: it captures 16-bit stereo at the port's rate and
/// sends it down the audio socket as it comes.
///
/// A read takes as long as the sound it returns, as it does on the console: titles read in a
/// loop on a thread of their own and rely on the call for the pace. The clock for that is this
/// side's, not the microphone's, so a read never waits for a host that has nothing to send
/// (no microphone, no permission to use it, no host at all): what is missing is silence.
class BachataInPortBackend final : public PortInBackend {
public:
    explicit BachataInPortBackend(const PortIn& port)
        : frames{port.samples_num}, rate{port.freq}, channels{port.channels_num},
          period{std::chrono::nanoseconds{1'000'000'000LL * port.samples_num / port.freq}} {
        const char* socket_path = std::getenv("BACHATA_ALSA_SOCKET");
        if (socket_path != nullptr && socket_path[0] == '/') {
            transport = Platform::Bachata::AudioTransport::Connect(socket_path);
            if (transport && !transport->Capture(rate)) {
                transport.reset();
            }
        }
        if (transport) {
            LOG_INFO(Lib_AudioIn, "Microphone asked of the host: {} Hz, {} channels", rate,
                     channels);
        } else {
            LOG_INFO(Lib_AudioIn, "No host to ask for a microphone: the port hears silence");
        }
        next = Clock::now();
    }

    int Read(void* out_buffer) override {
        const auto now = Clock::now();
        if (now - next > period * 16) {
            // The title has not read for a while; what arrived meanwhile is old.
            next = now;
            Receive();
            pending.clear();
            priming = true;
        }
        next += period;
        std::this_thread::sleep_until(next);
        Receive();

        // The host's clock and this one differ a little. Sound that piles up is delay: beyond
        // a tenth of a second the oldest of it goes.
        const std::size_t block = std::size_t{frames} * HostFrame;
        const std::size_t limit = std::size_t{rate} / 10 * HostFrame;
        if (pending.size() > limit + block) {
            const std::size_t excess = (pending.size() - limit) / HostFrame * HostFrame;
            pending.erase(pending.begin(), pending.begin() + static_cast<std::ptrdiff_t>(excess));
        }
        // After running dry, a little is let to gather first: without it every block would
        // end in a gap while the host delivers in pieces of another size.
        if (priming && pending.size() >= block * 4) {
            priming = false;
        }

        auto* out = static_cast<s16*>(out_buffer);
        const std::size_t available = priming ? 0 : std::min(pending.size(), block) / HostFrame;
        for (std::size_t frame = 0; frame < frames; ++frame) {
            s16 pair[2]{};
            if (frame < available) {
                std::memcpy(pair, pending.data() + frame * HostFrame, HostFrame);
            }
            if (channels == 1) {
                out[frame] = static_cast<s16>((pair[0] + pair[1]) / 2);
            } else {
                out[frame * 2] = pair[0];
                out[frame * 2 + 1] = pair[1];
            }
        }
        pending.erase(pending.begin(),
                      pending.begin() + static_cast<std::ptrdiff_t>(available * HostFrame));
        if (!priming && available < frames && transport) {
            priming = true;
        }
        return static_cast<int>(frames);
    }

    void Clear() override {
        Receive();
        pending.clear();
        priming = true;
    }

    bool IsAvailable() override {
        return true;
    }

private:
    using Clock = std::chrono::steady_clock;
    /// Bytes of a frame as the host sends it.
    static constexpr std::size_t HostFrame = 2 * sizeof(s16);

    void Receive() {
        while (transport) {
            const std::size_t size = pending.size();
            pending.resize(size + 4096);
            const auto count = transport->Read({pending.data() + size, 4096});
            pending.resize(size + static_cast<std::size_t>(std::max<std::ptrdiff_t>(count, 0)));
            if (count < 0) {
                LOG_WARNING(Lib_AudioIn, "The host's microphone is gone");
                transport.reset();
            }
            if (count <= 0) {
                break;
            }
        }
    }

    std::optional<Platform::Bachata::AudioTransport> transport;
    std::vector<std::uint8_t> pending;
    u32 frames;
    u32 rate;
    u32 channels;
    std::chrono::nanoseconds period;
    Clock::time_point next;
    bool priming{true};
};

} // namespace

std::unique_ptr<PortInBackend> BachataAudioIn::Open(PortIn& port) {
    return std::make_unique<BachataInPortBackend>(port);
}

} // namespace Libraries::AudioIn
