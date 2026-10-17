// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <vector>
#include <optional>
#include <shared_mutex>
#include <stop_token>
#include <thread>
#include <core/emulator_settings.h>
#include <magic_enum/magic_enum.hpp>
#include "common/assert.h"
#include "common/logging/log.h"
#include "common/path_util.h"
#include "common/thread.h"
#include "common/wave_dump.h"
#include "core/libraries/audio/audioout.h"
#include "core/libraries/audio/audioout_backend.h"
#include "core/libraries/audio/audioout_error.h"
#include "core/libraries/audio/surround_virtualizer.h"
#include "core/libraries/kernel/time.h"
#include "core/libraries/libs.h"
#include "core/vr/vr_runtime.h"

namespace Libraries::AudioOut {

// Port table with shared_ptr - use std::shared_mutex for RW locking
std::array<std::shared_ptr<PortOut>, ORBIS_AUDIO_OUT_NUM_PORTS> port_table{};
std::shared_mutex port_table_mutex;
std::mutex port_allocation_mutex;

static std::unique_ptr<AudioOutBackend> audio;
static std::atomic<int> lazy_init{0};

// Port allocation ranges
constexpr struct PortRange {
    s32 start;
    s32 end;
    s32 count() const {
        return end - start + 1;
    }
} port_ranges[] = {
    {0, 7},   // MAIN
    {8, 8},   // BGM
    {9, 12},  // VOICE
    {13, 16}, // PERSONAL
    {17, 20}, // PADSPK
    {21, 21}, // Type 5-8
    {22, 22}, // Audio3d (126)
    {23, 23}, // AUX (127)
    {24, 24}, // Type 125
};

static AudioFormatInfo GetFormatInfo(const OrbisAudioOutParamFormat format) {
    static constexpr std::array<AudioFormatInfo, 8> format_infos = {{
        // S16Mono
        {false, 2, 1, {0}, false},
        // S16Stereo
        {false, 2, 2, {0, 1}, false},
        // S16_8CH
        {false, 2, 8, {0, 1, 2, 3, 4, 5, 6, 7}, false},
        // FloatMono
        {true, 4, 1, {0}, false},
        // FloatStereo
        {true, 4, 2, {0, 1}, false},
        // Float_8CH
        {true, 4, 8, {0, 1, 2, 3, 4, 5, 6, 7}, false},
        // S16_8CH_Std
        {false, 2, 8, {0, 1, 2, 3, 6, 7, 4, 5}, true},
        // Float_8CH_Std
        {true, 4, 8, {0, 1, 2, 3, 6, 7, 4, 5}, true},
    }};
    const auto index = static_cast<u32>(format);
    ASSERT_MSG(index < format_infos.size(), "Unknown audio format {}", index);
    return format_infos[index];
}

/*
 * Helper functions
 **/
static int GetPortRange(OrbisAudioOutPort type) {
    s32 _type = static_cast<s32>(type);

    switch (_type) {
    case 0:
        return 0; // MAIN
    case 1:
        return 1; // BGM
    case 2:
        return 2; // VOICE
    case 3:
        return 3; // PERSONAL
    case 4:
        return 4; // PADSPK
    case 5:
        return 5; // Type 5
    case 6:
        return 5; // Type 6
    case 7:
        return 5; // Type 7
    case 8:
        return 5; // Type 8
    case 126:
        return 6; // Audio3d
    case 125:
        return 8; // Type 125
    case 127:
        return 7; // AUX
    default:
        return -1;
    }
}

static int GetPortId(s32 handle) {
    int port_id = handle & 0xFF;

    if (port_id >= ORBIS_AUDIO_OUT_NUM_PORTS) {
        LOG_ERROR(Lib_AudioOut, "Invalid port");
        return ORBIS_AUDIO_OUT_ERROR_INVALID_PORT;
    }

    if ((handle & 0x3F000000) != 0x20000000) {
        LOG_ERROR(Lib_AudioOut, "Invalid port");
        return ORBIS_AUDIO_OUT_ERROR_INVALID_PORT;
    }

    return port_id;
}

static s32 GetPortType(s32 handle) {
    return (handle >> 16) & 0xFF;
}

static int AllocatePort(OrbisAudioOutPort type) {
    int range_idx = GetPortRange(type);
    if (range_idx < 0) {
        return -1;
    }

    const auto& range = port_ranges[range_idx];
    for (int i = range.start; i <= range.end; i++) {
        std::shared_lock read_lock{port_table_mutex};
        if (!port_table[i]) {
            return i;
        }
    }
    return -1;
}

void AdjustVol() {
    if (lazy_init.load(std::memory_order_relaxed) == 0 && audio == nullptr) {
        return;
    }

    std::shared_lock read_lock{port_table_mutex};
    for (int i = 0; i < ORBIS_AUDIO_OUT_NUM_PORTS; i++) {
        if (auto port = port_table[i]) {
            std::unique_lock lock{port->mutex, std::try_to_lock};
            if (lock.owns_lock()) {
                port->impl->SetVolume(port->volume);
            }
        }
    }
}

/// When a port last played on a device that paces its writer (steady clock ticks), for how
/// long after that the title counts as paced by it, and how much of a delay it may make up.
static std::atomic<std::chrono::steady_clock::rep> device_paced_at{
    std::numeric_limits<std::chrono::steady_clock::rep>::min() / 2};
static constexpr std::chrono::milliseconds DevicePacedFor{200};
static constexpr std::chrono::milliseconds CatchUp{200};

static void AudioOutputThread(std::shared_ptr<PortOut> port, const std::stop_token& stop) {
    {
        const auto thread_name = fmt::format("shadPS4:AudioOutputThread:{}", fmt::ptr(port.get()));
        Common::SetCurrentThreadName(thread_name.c_str());
    }

    // One buffer per the time it lasts. Without a device behind it, time lost (the emulator
    // stalling) is not made up for: a burst of buffers afterwards would only be sound played in
    // a hurry. With one, see below.
    using Clock = std::chrono::steady_clock;
    const auto period =
        std::chrono::nanoseconds(1000000000ULL * port->buffer_frames / port->sample_rate);
    auto next_buffer = Clock::now();

    // SHADPS4_AUDIOOUT_DUMP=<seconds> records what the title hands to every port, as
    // <user>/audioout_<type>_<n>.wav.
    Common::WaveDump dump;
    Common::WaveDump binaural_dump;
    std::optional<SurroundVirtualizer> binaural;
    std::vector<float> binaural_buffer;
    if (const char* seconds = std::getenv("SHADPS4_AUDIOOUT_DUMP");
        seconds != nullptr && std::atoi(seconds) > 0) {
        static std::atomic<int> serial{0};
        const auto path = Common::FS::GetUserPath(Common::FS::PathType::UserDir) /
                          fmt::format("audioout_{}_{}.wav", magic_enum::enum_name(port->type),
                                      serial.fetch_add(1));
        dump.Open(path.string(), port->format_info.num_channels, port->sample_rate,
                  u64(std::atoi(seconds)) * port->sample_rate);
        if (port->format_info.num_channels == 8) {
            // What a headset wearer gets to hear of a surround port.
            auto binaural_path = path;
            binaural_path.replace_extension(".binaural.wav");
            binaural_dump.Open(binaural_path.string(), 2, port->sample_rate,
                               u64(std::atoi(seconds)) * port->sample_rate);
            binaural.emplace(port->format_info.channel_layout, port->sample_rate);
        }
    }

    // With a VR headset as the display, the AUX port carries what the television plays for the
    // people watching; the wearer hears the main port. Playing both would double every sound.
    const bool muted =
        port->type == OrbisAudioOutPort::Aux && Core::Vr::Runtime::Instance().IsHeadsetConnected();
    if (muted) {
        LOG_INFO(Lib_AudioOut, "the AUX port is the mix for the television and is not played");
    }

    // The buffer the title handed over is taken out from under the lock and played without it.
    // A backend may well block while playing (that is how a real device paces its writer), and
    // a title that feeds several ports in one call takes all their locks: holding one for the
    // length of a buffer made every such call last as many buffers as it had ports.
    std::vector<u8> playing(port->BufferSize());

    const char* stats_setting = std::getenv("SHADPS4_AUDIOOUT_STATS");
    const bool stats = stats_setting != nullptr && stats_setting[0] != '0';
    auto stats_since = Clock::now();
    u64 played = 0;
    u64 idle = 0;
    Clock::duration output_time{};

    while (true) {
        // A real audio device paces whoever writes to it: it takes sound when it has room, and
        // what it holds ahead of the speakers is all that stands between a title that is late
        // for a moment and a gap in the sound. Once a port of the title plays on such a device,
        // every port leaves the pace to it:
        //  - buffers are asked for a little early. A device runs by its own clock, never quite
        //    this one, and one that runs a hair fast would otherwise be starved now and then.
        //  - a title that fell behind may hand over what it missed at once. The device has room
        //    for exactly that, and gets back what it holds ahead; without it every delay eats
        //    into that for good, until each hiccup is heard.
        // The title hands the buffers of all its ports over together, so the ports that play
        // nothing must not hold back the one that does.
        const auto now = Clock::now();
        const bool paces = port->impl && port->impl->IsDevicePaced();
        if (paces) {
            device_paced_at.store(now.time_since_epoch().count(), std::memory_order_relaxed);
        }
        const bool device_paced =
            paces || now.time_since_epoch().count() -
                             device_paced_at.load(std::memory_order_relaxed) <
                         Clock::duration{DevicePacedFor}.count();
        const auto step = device_paced ? period - period / 100 : period;
        if (now < next_buffer) {
            Common::AccurateSleep(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                      next_buffer - now),
                                  nullptr, false);
        } else if (device_paced) {
            if (now - next_buffer > Clock::duration{CatchUp}) {
                next_buffer = now - Clock::duration{CatchUp};
            }
        } else if (now - next_buffer > period) {
            next_buffer = now;
        }

        bool have_buffer = false;
        {
            std::unique_lock lock{port->mutex};
            if (!port->impl || stop.stop_requested()) {
                break;
            }

            if (device_paced && !port->output_ready) {
                // Nothing to play yet: the schedule waits for the title, along with this.
                port->output_cv.wait_for(lock, period, [&] {
                    return port->output_ready || stop.stop_requested();
                });
            }
            if (port->output_ready || !device_paced) {
                next_buffer += step;
            }

            if (port->output_ready) {
                std::memcpy(playing.data(), port->output_buffer, playing.size());
                have_buffer = true;
                port->output_ready = false;
                port->last_output_time =
                    Kernel::sceKernelGetProcessTime(); // moved from sceAudioOutOutput TOOD recheck
            }
        }

        port->output_cv.notify_one();

        if (have_buffer) {
            if (dump.IsOpen()) {
                if (port->format_info.is_float) {
                    dump.Write(reinterpret_cast<const float*>(playing.data()),
                               port->buffer_frames);
                } else if (port->format_info.sample_size == 2) {
                    dump.Write(reinterpret_cast<const s16*>(playing.data()), port->buffer_frames);
                }
            }
            if (binaural) {
                binaural_buffer.resize(size_t{port->buffer_frames} * 2);
                if (port->format_info.is_float) {
                    binaural->Process(reinterpret_cast<const float*>(playing.data()),
                                      port->buffer_frames, binaural_buffer.data());
                } else {
                    binaural->Process(reinterpret_cast<const s16*>(playing.data()),
                                      port->buffer_frames, binaural_buffer.data());
                }
                binaural_dump.Write(binaural_buffer.data(), port->buffer_frames);
            }
            if (muted) {
                std::memset(playing.data(), 0, playing.size());
            }
            const auto output_begin = Clock::now();
            port->impl->Output(playing.data());
            output_time += Clock::now() - output_begin;
            ++played;
        } else {
            ++idle;
        }

        // SHADPS4_AUDIOOUT_STATS=1: every ten seconds, how much of real time the port played
        // and what kept it from more: the title not handing buffers over, or the device not
        // taking them.
        if (stats && Clock::now() - stats_since >= std::chrono::seconds{10}) {
            const double seconds = std::chrono::duration<double>(Clock::now() - stats_since).count();
            LOG_INFO(Lib_AudioOut,
                     "port {} ({}): {:.0f}% of real time played, {} rounds without a buffer, "
                     "{:.2f} ms per buffer in the output",
                     magic_enum::enum_name(port->type), fmt::ptr(port.get()),
                     100.0 * played * port->buffer_frames / port->sample_rate / seconds, idle,
                     played != 0
                         ? std::chrono::duration<double, std::milli>(output_time).count() / played
                         : 0.0);
            played = 0;
            idle = 0;
            output_time = {};
            stats_since = Clock::now();
        }

        if (stop.stop_requested()) {
            break;
        }
    }
}

/*
 * sceAudioOut implementation
 **/
s32 PS4_SYSV_ABI sceAudioOutInit() {
    LOG_TRACE(Lib_AudioOut, "called");

    int expected = 0;
    if (!lazy_init.compare_exchange_strong(expected, 1, std::memory_order_acq_rel)) {
        return ORBIS_AUDIO_OUT_ERROR_ALREADY_INIT;
    }

#ifdef ENABLE_BACHATA_RUNTIME
    audio = std::make_unique<BachataAudioOut>();
#else
    if (EmulatorSettings.GetAudioBackend() == AudioBackend::OpenAL) {
        audio = std::make_unique<OpenALAudioOut>();
    } else {
        audio = std::make_unique<SDLAudioOut>();
    }
#endif

    LOG_INFO(Lib_AudioOut, "Audio system initialized");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceAudioOutOpen(UserService::OrbisUserServiceUserId user_id,
                                 OrbisAudioOutPort port_type, s32 index, u32 length,
                                 u32 sample_rate,
                                 OrbisAudioOutParamExtendedInformation param_type) {
    LOG_INFO(Lib_AudioOut,
             "called, user_id={}, port_type={}({}), index={}, length={}, "
             "sample_rate={}, data_format={}({}), attributes={}({})",
             user_id, magic_enum::enum_name(port_type), static_cast<u32>(port_type), index, length,
             sample_rate, magic_enum::enum_name(param_type.data_format.Value()),
             static_cast<u32>(param_type.data_format.Value()),
             magic_enum::enum_name(param_type.attributes.Value()),
             static_cast<u32>(param_type.attributes.Value()));

    if (lazy_init.load(std::memory_order_relaxed) == 0 || audio == nullptr) {
        LOG_ERROR(Lib_AudioOut, "Audio out not initialized");
        return ORBIS_AUDIO_OUT_ERROR_NOT_INIT;
    }

    if (length == 0 || length > 2048 || (length & 0xFF) != 0) {
        LOG_ERROR(Lib_AudioOut, "Invalid size");
        return ORBIS_AUDIO_OUT_ERROR_INVALID_SIZE;
    }

    s32 _type = static_cast<s32>(port_type);
    u32 param_raw = param_type.Unpack();

    // Extract attributes
    bool is_restricted = (param_raw & ORBIS_AUDIO_OUT_PARAM_ATTR_RESTRICTED) != 0;
    bool is_mix_to_main = (param_raw & ORBIS_AUDIO_OUT_PARAM_ATTR_MIX_TO_MAIN) != 0;

    if (_type != 3 && is_mix_to_main) {
        LOG_ERROR(Lib_AudioOut, "Invalid format");
        return ORBIS_AUDIO_OUT_ERROR_INVALID_FORMAT;
    }

    if (_type != 0 && (param_raw & 0x70000000) != 0) {
        LOG_ERROR(Lib_AudioOut, "Invalid format");
        return ORBIS_AUDIO_OUT_ERROR_INVALID_FORMAT;
    }

    if ((port_type < OrbisAudioOutPort::Main || port_type > OrbisAudioOutPort::PadSpk) &&
        (port_type != OrbisAudioOutPort::Audio3d && port_type != OrbisAudioOutPort::Aux)) {
        LOG_ERROR(Lib_AudioOut, "Invalid port type");
        return ORBIS_AUDIO_OUT_ERROR_INVALID_PORT_TYPE;
    }
    if (sample_rate != 48000) {
        LOG_ERROR(Lib_AudioOut, "Invalid sample rate");
        return ORBIS_AUDIO_OUT_ERROR_INVALID_SAMPLE_FREQ;
    }

    if (index != 0) {
        LOG_ERROR(Lib_AudioOut, "index is not valid !=0 {}", index);
    }

    const auto format = param_type.data_format.Value();
    if (format < OrbisAudioOutParamFormat::S16Mono ||
        format > OrbisAudioOutParamFormat::Float_8CH_Std) {
        LOG_ERROR(Lib_AudioOut, "Invalid format");
        return ORBIS_AUDIO_OUT_ERROR_INVALID_FORMAT;
    }
    const auto attr = param_type.attributes;
    if (attr < OrbisAudioOutParamAttr::None || attr > OrbisAudioOutParamAttr::MixToMain) {
        // TODO Handle attributes in output audio device
        LOG_ERROR(Lib_AudioOut, "Invalid format attribute");
        return ORBIS_AUDIO_OUT_ERROR_INVALID_FORMAT;
    }

    std::unique_lock lock{port_allocation_mutex};

    // Allocate port
    int port_id = AllocatePort(port_type);
    if (port_id < 0) {
        LOG_ERROR(Lib_AudioOut, "Error allocated port");
        return ORBIS_AUDIO_OUT_ERROR_PORT_FULL;
    }

    // Create port object
    std::shared_ptr<PortOut> port;
    try {
        port = std::make_shared<PortOut>();

        port->userId = user_id;
        port->type = static_cast<OrbisAudioOutPort>(_type);
        port->format_info = GetFormatInfo(param_type.data_format.Value());
        port->sample_rate = sample_rate;
        port->buffer_frames = length;
        port->volume.fill(ORBIS_AUDIO_OUT_VOLUME_0DB);
        port->mixLevelPadSpk = ORBIS_AUDIO_OUT_MIXLEVEL_PADSPK_DEFAULT;

        // Set attributes
        port->is_restricted = is_restricted;
        port->is_mix_to_main = is_mix_to_main;

        // Log attributes if present
        if (is_restricted) {
            LOG_INFO(Lib_AudioOut, "Audio port opened with RESTRICTED attribute");
        }
        if (is_mix_to_main) {
            LOG_INFO(Lib_AudioOut, "Audio port opened with MIX_TO_MAIN attribute");
        }

        // Create backend
        port->impl = audio->Open(*port);
        if (!port->impl) {
            throw std::runtime_error("Failed to create audio backend");
        }

        // Allocate buffer
        port->output_buffer = std::malloc(port->BufferSize());
        if (!port->output_buffer) {
            throw std::bad_alloc();
        }

        // Start output thread - pass shared_ptr by value to keep port alive
        port->output_thread.Run(
            [port](const std::stop_token& stop) { AudioOutputThread(port, stop); });

        // Set initial volume
        port->impl->SetVolume(port->volume);

    } catch (const std::bad_alloc&) {
        return ORBIS_AUDIO_OUT_ERROR_OUT_OF_MEMORY;
    } catch (const std::exception& e) {
        LOG_ERROR(Lib_AudioOut, "Failed to open audio port: {}", e.what());
        return ORBIS_AUDIO_OUT_ERROR_TRANS_EVENT;
    }

    {
        std::unique_lock write_lock{port_table_mutex};
        port_table[port_id] = port;
    }

    // Create handle
    s32 handle = (_type << 16) | port_id | 0x20000000;
    return handle;
}

#ifdef SHADPS4_ENABLE_FEX_GUEST_CPU
s32 PS4_SYSV_ABI fex_sceAudioOutOpen(UserService::OrbisUserServiceUserId user_id,
                                     OrbisAudioOutPort port_type, s32 index, u32 length,
                                     u32 sample_rate, u32 param_raw) {
    OrbisAudioOutParamExtendedInformation param_type{};
    static_assert(sizeof(param_type) == sizeof(param_raw));
    std::memcpy(&param_type, &param_raw, sizeof(param_type));
    return sceAudioOutOpen(user_id, port_type, index, length, sample_rate, param_type);
}
#endif

s32 PS4_SYSV_ABI sceAudioOutClose(s32 handle) {
    LOG_INFO(Lib_AudioOut, "handle = {:#x}", handle);

    if (lazy_init.load(std::memory_order_relaxed) == 0 || audio == nullptr) {
        LOG_ERROR(Lib_AudioOut, "Audio out not initialized");
        return ORBIS_AUDIO_OUT_ERROR_NOT_INIT;
    }

    int port_id = GetPortId(handle);
    if (port_id < 0) {
        LOG_ERROR(Lib_AudioOut, "Invalid port id");
        return port_id;
    }

    s32 port_type = GetPortType(handle);
    if (port_type >= 5 && port_type <= 13) {
        LOG_ERROR(Lib_AudioOut, "Invalid port type");
        return ORBIS_AUDIO_OUT_ERROR_INVALID_PORT_TYPE;
    }

    // Check valid types
    if (!((port_type >= 0 && port_type <= 4) || port_type == 14 || port_type == 126 ||
          port_type == 127)) {
        LOG_ERROR(Lib_AudioOut, "Invalid port type");
        return ORBIS_AUDIO_OUT_ERROR_INVALID_PORT_TYPE;
    }

    std::unique_lock lock{port_allocation_mutex};

    std::shared_ptr<PortOut> port;
    {
        std::unique_lock write_lock{port_table_mutex};
        port = std::move(port_table[port_id]);
        port_table[port_id].reset();
    }

    if (!port) {
        LOG_ERROR(Lib_AudioOut, "Port wasn't open {}", port_id);
        return ORBIS_AUDIO_OUT_ERROR_NOT_OPENED;
    }

    // Stop the output thread
    port->output_thread.Stop();

    std::free(port->output_buffer);

    LOG_DEBUG(Lib_AudioOut, "Closed audio port {}", port_id);
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceAudioOutGetLastOutputTime(s32 handle, u64* output_time) {
    if (lazy_init.load(std::memory_order_relaxed) == 0 || audio == nullptr) {
        LOG_ERROR(Lib_AudioOut, "audio is not init");
        return ORBIS_AUDIO_OUT_ERROR_NOT_INIT;
    }

    int port_id = GetPortId(handle);
    if (port_id < 0) {
        LOG_ERROR(Lib_AudioOut, "Invalid port id");
        return port_id;
    }

    if (!output_time) {
        LOG_ERROR(Lib_AudioOut, "Invalid pointer");
        return ORBIS_AUDIO_OUT_ERROR_INVALID_POINTER;
    }

    std::shared_ptr<PortOut> port;
    {
        std::shared_lock read_lock{port_table_mutex};
        port = port_table[port_id];
    }

    if (!port) {
        LOG_ERROR(Lib_AudioOut, "Port not opened {}", port_id);
        return ORBIS_AUDIO_OUT_ERROR_NOT_OPENED;
    }

    std::unique_lock lock{port->mutex};
    *output_time = port->last_output_time;

    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceAudioOutGetPortState(s32 handle, OrbisAudioOutPortState* state) {
    if (lazy_init.load(std::memory_order_relaxed) == 0 || audio == nullptr) {
        LOG_ERROR(Lib_AudioOut, "audio is not init");
        return ORBIS_AUDIO_OUT_ERROR_NOT_INIT;
    }

    int port_id = GetPortId(handle);
    if (port_id < 0) {
        LOG_ERROR(Lib_AudioOut, "Invalid port id");
        return port_id;
    }

    if (!state) {
        LOG_ERROR(Lib_AudioOut, "Invalid pointer");
        return ORBIS_AUDIO_OUT_ERROR_INVALID_POINTER;
    }

    std::shared_ptr<PortOut> port;
    {
        std::shared_lock read_lock{port_table_mutex};
        port = port_table[port_id];
    }

    if (!port) {
        LOG_ERROR(Lib_AudioOut, "Port is not open {}", port_id);
        return ORBIS_AUDIO_OUT_ERROR_NOT_OPENED;
    }

    std::unique_lock lock{port->mutex};

    switch (port->type) {
    case OrbisAudioOutPort::Main:
    case OrbisAudioOutPort::Bgm:
    case OrbisAudioOutPort::Audio3d:
        state->output = ORBIS_AUDIO_OUT_STATE_OUTPUT_CONNECTED_PRIMARY;
        state->channel = port->format_info.num_channels > 2 ? 2 : port->format_info.num_channels;
        break;
    case OrbisAudioOutPort::Voice:
    case OrbisAudioOutPort::Personal:
        state->output = ORBIS_AUDIO_OUT_STATE_OUTPUT_CONNECTED_HEADPHONE;
        state->channel = 1;
        break;
    case OrbisAudioOutPort::PadSpk:
        state->output = ORBIS_AUDIO_OUT_STATE_OUTPUT_CONNECTED_TERTIARY;
        state->channel = 1;
        state->volume = 127; // max
        break;
    case OrbisAudioOutPort::Aux:
        state->output = ORBIS_AUDIO_OUT_STATE_OUTPUT_CONNECTED_EXTERNAL;
        state->channel = 0;
        break;
    default:
        state->output = ORBIS_AUDIO_OUT_STATE_OUTPUT_UNKNOWN;
        state->channel = 0;
        break;
    }

    if (port->type != OrbisAudioOutPort::PadSpk) {
        state->volume = -1; // invalid
    }

    state->rerouteCounter = 0;
    state->flag = 0;
    LOG_DEBUG(Lib_AudioOut,
              "called, handle={:#x}, state={}, output={}, channel={}, volume={}, "
              "rerouteCounter={}, flag={}",
              handle, fmt::ptr(state), state->output, state->channel, state->volume,
              state->rerouteCounter, state->flag);
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceAudioOutOutput(s32 handle, void* ptr) {
    LOG_TRACE(Lib_AudioOut, "(STUBBED) called, handle={:#x}, ptr={}", handle, fmt::ptr(ptr));

    if (lazy_init.load(std::memory_order_relaxed) == 0 || audio == nullptr) {
        LOG_ERROR(Lib_AudioOut, "audio is not init");
        return ORBIS_AUDIO_OUT_ERROR_NOT_INIT;
    }

    int port_id = GetPortId(handle);
    if (port_id < 0) {
        LOG_ERROR(Lib_AudioOut, "invalid port id");
        return port_id;
    }

    s32 port_type = GetPortType(handle);
    if (port_type >= 5 && port_type <= 13) {
        LOG_ERROR(Lib_AudioOut, "Invalid port type");
        return ORBIS_AUDIO_OUT_ERROR_INVALID_PORT_TYPE;
    }

    // Check valid types
    if (!((port_type >= 0 && port_type <= 4) || port_type == 14 || port_type == 126 ||
          port_type == 127)) {
        LOG_ERROR(Lib_AudioOut, "Invalid port type");
        return ORBIS_AUDIO_OUT_ERROR_INVALID_PORT_TYPE;
    }

    std::shared_ptr<PortOut> port;
    {
        std::shared_lock read_lock{port_table_mutex};
        port = port_table[port_id];
    }

    if (!port) {
        LOG_ERROR(Lib_AudioOut, "Port is not opened {}", port_id);
        return ORBIS_AUDIO_OUT_ERROR_NOT_OPENED;
    }

    s32 samples_sent = 0;
    {
        std::unique_lock lock{port->mutex};
        port->output_cv.wait(lock, [&] { return !port->output_ready; });

        if (ptr != nullptr) {
            std::memcpy(port->output_buffer, ptr, port->BufferSize());
            port->output_ready = true;
            samples_sent = port->buffer_frames * port->format_info.num_channels;
        }
    }
    // The port's thread may be waiting for exactly this.
    port->output_cv.notify_all();

    return samples_sent;
}

s32 PS4_SYSV_ABI sceAudioOutOutputs(OrbisAudioOutOutputParam* param, u32 num) {
    if (param) {
        LOG_TRACE(Lib_AudioOut, "(STUBBED) called, param={}, num={}", fmt::ptr(param), num);
        for (u32 i = 0; i < num; i++) {
            LOG_TRACE(Lib_AudioOut, "  [{}] handle={:#x}, ptr={}", i, param[i].handle,
                      fmt::ptr(param[i].ptr));
        }
    } else {
        LOG_TRACE(Lib_AudioOut, "(STUBBED) called, param=nullptr, num={}", num);
    }
    if (lazy_init.load(std::memory_order_relaxed) == 0 || audio == nullptr) {
        LOG_ERROR(Lib_AudioOut, "audio is not init");
        return ORBIS_AUDIO_OUT_ERROR_NOT_INIT;
    }

    if (num == 0 || num > 25) {
        LOG_ERROR(Lib_AudioOut, "ports is full");
        return ORBIS_AUDIO_OUT_ERROR_PORT_FULL;
    }

    if (!param) {
        LOG_ERROR(Lib_AudioOut, "invalid pointer");
        return ORBIS_AUDIO_OUT_ERROR_INVALID_POINTER;
    }

    std::vector<std::shared_ptr<PortOut>> ports;
    std::vector<std::unique_lock<std::mutex>> locks;
    ports.reserve(num);
    locks.reserve(num);

    u32 buffer_frames = 0;

    {
        std::shared_lock read_lock{port_table_mutex};

        for (u32 i = 0; i < num; i++) {
            int port_id = GetPortId(param[i].handle);
            if (port_id < 0) {
                LOG_ERROR(Lib_AudioOut, "invalid port id");
                return port_id;
            }

            s32 port_type = GetPortType(param[i].handle);
            if (port_type >= 5 && port_type <= 13) {
                LOG_ERROR(Lib_AudioOut, "Invalid port type");
                return ORBIS_AUDIO_OUT_ERROR_INVALID_PORT_TYPE;
            }

            // Check valid types
            if (!((port_type >= 0 && port_type <= 4) || port_type == 14 || port_type == 126 ||
                  port_type == 127)) {
                LOG_ERROR(Lib_AudioOut, "Invalid port type");
                return ORBIS_AUDIO_OUT_ERROR_INVALID_PORT_TYPE;
            }

            // Check for duplicate handles
            for (u32 j = 0; j < i; j++) {
                if (param[i].handle == param[j].handle) {
                    LOG_ERROR(Lib_AudioOut, "Duplicate audio handles: {:#x}", param[i].handle);
                    return ORBIS_AUDIO_OUT_ERROR_INVALID_PORT;
                }
            }

            // Get port
            auto port = port_table[port_id];
            if (!port) {
                LOG_ERROR(Lib_AudioOut, "Port not opened {}", port_id);
                return ORBIS_AUDIO_OUT_ERROR_NOT_OPENED;
            }

            ports.push_back(port);
            locks.emplace_back(port->mutex);

            // Check consistent buffer size
            if (i == 0) {
                buffer_frames = port->buffer_frames;
            } else if (port->buffer_frames != buffer_frames) {
                LOG_ERROR(Lib_AudioOut, "Invalid port size");
                return ORBIS_AUDIO_OUT_ERROR_INVALID_SIZE;
            }
        }
    }

    // Wait for all ports to be ready
    for (u32 i = 0; i < num; i++) {
        ports[i]->output_cv.wait(locks[i], [&] { return !ports[i]->output_ready; });
    }

    // Copy data to all ports
    for (u32 i = 0; i < num; i++) {
        if (param[i].ptr != nullptr) {
            std::memcpy(ports[i]->output_buffer, param[i].ptr, ports[i]->BufferSize());
            ports[i]->output_ready = true;
        }
    }
    // The ports' threads may be waiting for exactly this.
    locks.clear();
    for (u32 i = 0; i < num; i++) {
        ports[i]->output_cv.notify_all();
    }

    return buffer_frames;
}

s32 PS4_SYSV_ABI sceAudioOutSetVolume(s32 handle, s32 flag, s32* vol) {
    if (lazy_init.load(std::memory_order_relaxed) == 0 || audio == nullptr) {
        LOG_ERROR(Lib_AudioOut, "audio is not init");
        return ORBIS_AUDIO_OUT_ERROR_NOT_INIT;
    }

    int port_id = GetPortId(handle);
    if (port_id < 0) {
        LOG_ERROR(Lib_AudioOut, "invalid port_id");
        return port_id;
    }

    s32 port_type = GetPortType(handle);
    if (port_type >= 5 && port_type <= 13) {
        LOG_ERROR(Lib_AudioOut, "Invalid port type");
        return ORBIS_AUDIO_OUT_ERROR_INVALID_PORT_TYPE;
    }

    // Check valid types
    if (!((port_type >= 0 && port_type <= 4) || port_type == 14 || port_type == 126 ||
          port_type == 127)) {
        LOG_ERROR(Lib_AudioOut, "Invalid port type");
        return ORBIS_AUDIO_OUT_ERROR_INVALID_PORT_TYPE;
    }

    if (!vol) {
        LOG_ERROR(Lib_AudioOut, "Invalid pointer");
        return ORBIS_AUDIO_OUT_ERROR_INVALID_POINTER;
    }

    if (*vol > ORBIS_AUDIO_OUT_VOLUME_0DB) {
        LOG_ERROR(Lib_AudioOut, "Invalid volume");
        return ORBIS_AUDIO_OUT_ERROR_INVALID_VOLUME;
    }

    // Get port with shared lock (read-only access to table)
    std::shared_ptr<PortOut> port;
    {
        std::shared_lock read_lock{port_table_mutex};
        port = port_table[port_id];
    }

    if (!port) {
        LOG_ERROR(Lib_AudioOut, "Port not opened {}", port_id);
        return ORBIS_AUDIO_OUT_ERROR_NOT_OPENED;
    }

    std::unique_lock lock{port->mutex};

    // Set volumes based on flags
    if (flag & ORBIS_AUDIO_VOLUME_FLAG_L_CH)
        port->volume[0] = *vol;
    if (flag & ORBIS_AUDIO_VOLUME_FLAG_R_CH)
        port->volume[1] = *vol;
    if (flag & ORBIS_AUDIO_VOLUME_FLAG_C_CH)
        port->volume[2] = *vol;
    if (flag & ORBIS_AUDIO_VOLUME_FLAG_LFE_CH)
        port->volume[3] = *vol;
    if (flag & ORBIS_AUDIO_VOLUME_FLAG_LS_CH)
        port->volume[4] = *vol;
    if (flag & ORBIS_AUDIO_VOLUME_FLAG_RS_CH)
        port->volume[5] = *vol;
    if (flag & ORBIS_AUDIO_VOLUME_FLAG_LE_CH)
        port->volume[6] = *vol;
    if (flag & ORBIS_AUDIO_VOLUME_FLAG_RE_CH)
        port->volume[7] = *vol;

    port->impl->SetVolume(port->volume);

    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceAudioOutSetMixLevelPadSpk(s32 handle, s32 mixLevel) {
    LOG_INFO(Lib_AudioOut, "(STUBBED) called");
    if (lazy_init.load(std::memory_order_relaxed) == 0 || audio == nullptr) {
        LOG_ERROR(Lib_AudioOut, "audio is not init");
        return ORBIS_AUDIO_OUT_ERROR_NOT_INIT;
    }

    int port_id = GetPortId(handle);
    if (port_id < 0) {
        LOG_ERROR(Lib_AudioOut, "Invalid port_id");
        return port_id;
    }

    if (GetPortType(handle) != 4) { // PadSpk
        LOG_ERROR(Lib_AudioOut, "Invalid port type");
        return ORBIS_AUDIO_OUT_ERROR_INVALID_PORT_TYPE;
    }

    if (mixLevel > ORBIS_AUDIO_OUT_VOLUME_0DB) {
        LOG_ERROR(Lib_AudioOut, "Invalid mix level");
        return ORBIS_AUDIO_OUT_ERROR_INVALID_MIXLEVEL;
    }

    std::shared_ptr<PortOut> port;
    {
        std::shared_lock read_lock{port_table_mutex};
        port = port_table[port_id];
    }

    if (!port) {
        LOG_ERROR(Lib_AudioOut, "Port not opened");
        return ORBIS_AUDIO_OUT_ERROR_NOT_OPENED;
    }

    std::unique_lock lock{port->mutex};
    port->mixLevelPadSpk = mixLevel;
    // TODO: Apply mix level to backend

    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceAudioOutGetSystemState(OrbisAudioOutSystemState* state) {
    if (lazy_init.load(std::memory_order_relaxed) == 0 || audio == nullptr) {
        LOG_ERROR(Lib_AudioOut, "audio is not init");
        return ORBIS_AUDIO_OUT_ERROR_NOT_INIT;
    }
    if (state == nullptr) {
        return ORBIS_AUDIO_OUT_ERROR_INVALID_POINTER;
    }
    memset(state, 0, sizeof(*state));
    LOG_DEBUG(Lib_AudioOut, "called");
    return ORBIS_OK;
}

/*
 * Stubbed functions
 **/
s32 PS4_SYSV_ABI sceAudioOutDeviceIdOpen() {
    LOG_ERROR(Lib_AudioOut, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceAudioDeviceControlGet() {
    LOG_ERROR(Lib_AudioOut, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceAudioDeviceControlSet() {
    LOG_ERROR(Lib_AudioOut, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceAudioOutA3dControl() {
    LOG_ERROR(Lib_AudioOut, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceAudioOutA3dExit() {
    LOG_ERROR(Lib_AudioOut, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceAudioOutA3dInit() {
    LOG_ERROR(Lib_AudioOut, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceAudioOutAttachToApplicationByPid() {
    LOG_ERROR(Lib_AudioOut, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceAudioOutChangeAppModuleState() {
    LOG_ERROR(Lib_AudioOut, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceAudioOutDetachFromApplicationByPid() {
    LOG_ERROR(Lib_AudioOut, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceAudioOutExConfigureOutputMode() {
    LOG_ERROR(Lib_AudioOut, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceAudioOutExGetSystemInfo() {
    LOG_ERROR(Lib_AudioOut, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceAudioOutExPtClose() {
    LOG_ERROR(Lib_AudioOut, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceAudioOutExPtGetLastOutputTime() {
    LOG_ERROR(Lib_AudioOut, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceAudioOutExPtOpen() {
    LOG_ERROR(Lib_AudioOut, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceAudioOutExSystemInfoIsSupportedAudioOutExMode() {
    LOG_ERROR(Lib_AudioOut, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceAudioOutGetFocusEnablePid() {
    LOG_ERROR(Lib_AudioOut, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceAudioOutGetHandleStatusInfo() {
    LOG_ERROR(Lib_AudioOut, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceAudioOutGetInfo() {
    LOG_ERROR(Lib_AudioOut, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceAudioOutGetInfoOpenNum() {
    LOG_ERROR(Lib_AudioOut, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceAudioOutOpenEx() {
    LOG_ERROR(Lib_AudioOut, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceAudioOutGetSimulatedBusUsableStatusByBusType() {
    LOG_ERROR(Lib_AudioOut, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceAudioOutGetSimulatedHandleStatusInfo() {
    LOG_ERROR(Lib_AudioOut, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceAudioOutGetSimulatedHandleStatusInfo2() {
    LOG_ERROR(Lib_AudioOut, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceAudioOutGetSparkVss() {
    LOG_ERROR(Lib_AudioOut, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceAudioOutInitIpmiGetSession() {
    LOG_ERROR(Lib_AudioOut, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceAudioOutMasteringGetState() {
    LOG_ERROR(Lib_AudioOut, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceAudioOutMasteringInit(u32 flags) {
    LOG_ERROR(Lib_AudioOut, "(STUBBED) called");
    if (flags != 0) {
        return ORBIS_AUDIO_OUT_ERROR_MASTERING_INVALID_API_PARAM;
    }
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceAudioOutMasteringSetParam() {
    LOG_ERROR(Lib_AudioOut, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceAudioOutMasteringTerm() {
    LOG_ERROR(Lib_AudioOut, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceAudioOutMbusInit() {
    LOG_ERROR(Lib_AudioOut, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceAudioOutPtClose() {
    LOG_ERROR(Lib_AudioOut, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceAudioOutPtGetLastOutputTime() {
    LOG_ERROR(Lib_AudioOut, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceAudioOutPtOpen() {
    LOG_ERROR(Lib_AudioOut, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceAudioOutSetConnections() {
    LOG_ERROR(Lib_AudioOut, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceAudioOutSetConnectionsForUser() {
    LOG_ERROR(Lib_AudioOut, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceAudioOutSetDevConnection() {
    LOG_ERROR(Lib_AudioOut, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceAudioOutSetHeadphoneOutMode() {
    LOG_ERROR(Lib_AudioOut, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceAudioOutSetJediJackVolume() {
    LOG_ERROR(Lib_AudioOut, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceAudioOutSetJediSpkVolume() {
    LOG_ERROR(Lib_AudioOut, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceAudioOutSetMainOutput() {
    LOG_ERROR(Lib_AudioOut, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceAudioOutSetMorpheusParam() {
    LOG_ERROR(Lib_AudioOut, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceAudioOutSetMorpheusWorkingMode() {
    LOG_ERROR(Lib_AudioOut, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceAudioOutSetPortConnections() {
    LOG_ERROR(Lib_AudioOut, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceAudioOutSetPortStatuses() {
    LOG_ERROR(Lib_AudioOut, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceAudioOutSetRecMode() {
    LOG_ERROR(Lib_AudioOut, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceAudioOutSetSparkParam() {
    LOG_ERROR(Lib_AudioOut, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceAudioOutSetUsbVolume() {
    LOG_ERROR(Lib_AudioOut, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceAudioOutSetVolumeDown() {
    LOG_ERROR(Lib_AudioOut, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceAudioOutStartAuxBroadcast() {
    LOG_ERROR(Lib_AudioOut, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceAudioOutStartSharePlay() {
    LOG_ERROR(Lib_AudioOut, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceAudioOutStopAuxBroadcast() {
    LOG_ERROR(Lib_AudioOut, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceAudioOutStopSharePlay() {
    LOG_ERROR(Lib_AudioOut, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceAudioOutSuspendResume() {
    LOG_ERROR(Lib_AudioOut, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceAudioOutSysConfigureOutputMode() {
    LOG_ERROR(Lib_AudioOut, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceAudioOutSysGetHdmiMonitorInfo() {
    LOG_ERROR(Lib_AudioOut, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceAudioOutSysGetSystemInfo() {
    LOG_ERROR(Lib_AudioOut, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceAudioOutSysHdmiMonitorInfoIsSupportedAudioOutMode() {
    LOG_ERROR(Lib_AudioOut, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceAudioOutSystemControlGet() {
    LOG_ERROR(Lib_AudioOut, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceAudioOutSystemControlSet() {
    LOG_ERROR(Lib_AudioOut, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceAudioOutSparkControlSetEqCoef() {
    LOG_ERROR(Lib_AudioOut, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceAudioOutSetSystemDebugState() {
    LOG_ERROR(Lib_AudioOut, "(STUBBED) called");
    return ORBIS_OK;
}

void RegisterLib(Core::Loader::SymbolsResolver* sym) {
    LIB_FUNCTION("cx2dYFbzIAg", "libSceAudioOutDeviceService", 1, "libSceAudioOut",
                 sceAudioOutDeviceIdOpen);
    LIB_FUNCTION("tKumjQSzhys", "libSceAudioDeviceControl", 1, "libSceAudioOut",
                 sceAudioDeviceControlGet);
    LIB_FUNCTION("5ChfcHOf3SM", "libSceAudioDeviceControl", 1, "libSceAudioOut",
                 sceAudioDeviceControlSet);
    LIB_FUNCTION("Iz9X7ISldhs", "libSceAudioOut", 1, "libSceAudioOut", sceAudioOutA3dControl);
    LIB_FUNCTION("9RVIoocOVAo", "libSceAudioOut", 1, "libSceAudioOut", sceAudioOutA3dExit);
    LIB_FUNCTION("n7KgxE8rOuE", "libSceAudioOut", 1, "libSceAudioOut", sceAudioOutA3dInit);
    LIB_FUNCTION("WBAO6-n0-4M", "libSceAudioOut", 1, "libSceAudioOut",
                 sceAudioOutAttachToApplicationByPid);
    LIB_FUNCTION("O3FM2WXIJaI", "libSceAudioOut", 1, "libSceAudioOut",
                 sceAudioOutChangeAppModuleState);
    LIB_FUNCTION("s1--uE9mBFw", "libSceAudioOut", 1, "libSceAudioOut", sceAudioOutClose);
    LIB_FUNCTION("ol4LbeTG8mc", "libSceAudioOut", 1, "libSceAudioOut",
                 sceAudioOutDetachFromApplicationByPid);
    LIB_FUNCTION("r1V9IFEE+Ts", "libSceAudioOut", 1, "libSceAudioOut",
                 sceAudioOutExConfigureOutputMode);
    LIB_FUNCTION("wZakRQsWGos", "libSceAudioOut", 1, "libSceAudioOut", sceAudioOutExGetSystemInfo);
    LIB_FUNCTION("xjjhT5uw08o", "libSceAudioOut", 1, "libSceAudioOut", sceAudioOutExPtClose);
    LIB_FUNCTION("DsST7TNsyfo", "libSceAudioOut", 1, "libSceAudioOut",
                 sceAudioOutExPtGetLastOutputTime);
    LIB_FUNCTION("4UlW3CSuCa4", "libSceAudioOut", 1, "libSceAudioOut", sceAudioOutExPtOpen);
    LIB_FUNCTION("Xcj8VTtnZw0", "libSceAudioOut", 1, "libSceAudioOut",
                 sceAudioOutExSystemInfoIsSupportedAudioOutExMode);
    LIB_FUNCTION("I3Fwcmkg5Po", "libSceAudioOut", 1, "libSceAudioOut",
                 sceAudioOutGetFocusEnablePid);
    LIB_FUNCTION("Y3lXfCFEWFY", "libSceAudioOut", 1, "libSceAudioOut",
                 sceAudioOutGetHandleStatusInfo);
    LIB_FUNCTION("-00OAutAw+c", "libSceAudioOut", 1, "libSceAudioOut", sceAudioOutGetInfo);
    LIB_FUNCTION("RqmKxBqB8B4", "libSceAudioOut", 1, "libSceAudioOut", sceAudioOutGetInfoOpenNum);
    LIB_FUNCTION("Ptlts326pds", "libSceAudioOut", 1, "libSceAudioOut",
                 sceAudioOutGetLastOutputTime);
    LIB_FUNCTION("GrQ9s4IrNaQ", "libSceAudioOut", 1, "libSceAudioOut", sceAudioOutGetPortState);
    LIB_FUNCTION("c7mVozxJkPU", "libSceAudioOut", 1, "libSceAudioOut",
                 sceAudioOutGetSimulatedBusUsableStatusByBusType);
    LIB_FUNCTION("pWmS7LajYlo", "libSceAudioOut", 1, "libSceAudioOut",
                 sceAudioOutGetSimulatedHandleStatusInfo);
    LIB_FUNCTION("oPLghhAWgMM", "libSceAudioOut", 1, "libSceAudioOut",
                 sceAudioOutGetSimulatedHandleStatusInfo2);
    LIB_FUNCTION("5+r7JYHpkXg", "libSceAudioOut", 1, "libSceAudioOut", sceAudioOutGetSparkVss);
    LIB_FUNCTION("R5hemoKKID8", "libSceAudioOut", 1, "libSceAudioOut", sceAudioOutGetSystemState);
    LIB_FUNCTION("JfEPXVxhFqA", "libSceAudioOut", 1, "libSceAudioOut", sceAudioOutInit);
    LIB_FUNCTION("n16Kdoxnvl0", "libSceAudioOut", 1, "libSceAudioOut",
                 sceAudioOutInitIpmiGetSession);
    LIB_FUNCTION("r+qKw+ueD+Q", "libSceAudioOut", 1, "libSceAudioOut",
                 sceAudioOutMasteringGetState);
    LIB_FUNCTION("xX4RLegarbg", "libSceAudioOut", 1, "libSceAudioOut", sceAudioOutMasteringInit);
    LIB_FUNCTION("4055yaUg3EY", "libSceAudioOut", 1, "libSceAudioOut",
                 sceAudioOutMasteringSetParam);
    LIB_FUNCTION("RVWtUgoif5o", "libSceAudioOut", 1, "libSceAudioOut", sceAudioOutMasteringTerm);
    LIB_FUNCTION("-LXhcGARw3k", "libSceAudioOut", 1, "libSceAudioOut", sceAudioOutMbusInit);
#ifdef SHADPS4_ENABLE_FEX_GUEST_CPU
    LIB_FUNCTION("ekNvsT22rsY", "libSceAudioOut", 1, "libSceAudioOut",
                 fex_sceAudioOutOpen);
#else
    LIB_FUNCTION("ekNvsT22rsY", "libSceAudioOut", 1, "libSceAudioOut", sceAudioOutOpen);
#endif
    LIB_FUNCTION("qLpSK75lXI4", "libSceAudioOut", 1, "libSceAudioOut", sceAudioOutOpenEx);
    LIB_FUNCTION("QOQtbeDqsT4", "libSceAudioOut", 1, "libSceAudioOut", sceAudioOutOutput);
    LIB_FUNCTION("w3PdaSTSwGE", "libSceAudioOut", 1, "libSceAudioOut", sceAudioOutOutputs);
    LIB_FUNCTION("MapHTgeogbk", "libSceAudioOut", 1, "libSceAudioOut", sceAudioOutPtClose);
    LIB_FUNCTION("YZaq+UKbriQ", "libSceAudioOut", 1, "libSceAudioOut",
                 sceAudioOutPtGetLastOutputTime);
    LIB_FUNCTION("xyT8IUCL3CI", "libSceAudioOut", 1, "libSceAudioOut", sceAudioOutPtOpen);
    LIB_FUNCTION("o4OLQQqqA90", "libSceAudioOut", 1, "libSceAudioOut", sceAudioOutSetConnections);
    LIB_FUNCTION("QHq2ylFOZ0k", "libSceAudioOut", 1, "libSceAudioOut",
                 sceAudioOutSetConnectionsForUser);
    LIB_FUNCTION("r9KGqGpwTpg", "libSceAudioOut", 1, "libSceAudioOut", sceAudioOutSetDevConnection);
    LIB_FUNCTION("08MKi2E-RcE", "libSceAudioOut", 1, "libSceAudioOut",
                 sceAudioOutSetHeadphoneOutMode);
    LIB_FUNCTION("18IVGrIQDU4", "libSceAudioOut", 1, "libSceAudioOut",
                 sceAudioOutSetJediJackVolume);
    LIB_FUNCTION("h0o+D4YYr1k", "libSceAudioOut", 1, "libSceAudioOut", sceAudioOutSetJediSpkVolume);
    LIB_FUNCTION("KI9cl22to7E", "libSceAudioOut", 1, "libSceAudioOut", sceAudioOutSetMainOutput);
    LIB_FUNCTION("wVwPU50pS1c", "libSceAudioOut", 1, "libSceAudioOut",
                 sceAudioOutSetMixLevelPadSpk);
    LIB_FUNCTION("eeRsbeGYe20", "libSceAudioOut", 1, "libSceAudioOut", sceAudioOutSetMorpheusParam);
    LIB_FUNCTION("IZrItPnflBM", "libSceAudioOut", 1, "libSceAudioOut",
                 sceAudioOutSetMorpheusWorkingMode);
    LIB_FUNCTION("Gy0ReOgXW00", "libSceAudioOut", 1, "libSceAudioOut",
                 sceAudioOutSetPortConnections);
    LIB_FUNCTION("oRBFflIrCg0", "libSceAudioOut", 1, "libSceAudioOut", sceAudioOutSetPortStatuses);
    LIB_FUNCTION("ae-IVPMSWjU", "libSceAudioOut", 1, "libSceAudioOut", sceAudioOutSetRecMode);
    LIB_FUNCTION("d3WL2uPE1eE", "libSceAudioOut", 1, "libSceAudioOut", sceAudioOutSetSparkParam);
    LIB_FUNCTION("X7Cfsiujm8Y", "libSceAudioOut", 1, "libSceAudioOut", sceAudioOutSetUsbVolume);
    LIB_FUNCTION("b+uAV89IlxE", "libSceAudioOut", 1, "libSceAudioOut", sceAudioOutSetVolume);
    LIB_FUNCTION("rho9DH-0ehs", "libSceAudioOut", 1, "libSceAudioOut", sceAudioOutSetVolumeDown);
    LIB_FUNCTION("I91P0HAPpjw", "libSceAudioOut", 1, "libSceAudioOut",
                 sceAudioOutStartAuxBroadcast);
    LIB_FUNCTION("uo+eoPzdQ-s", "libSceAudioOut", 1, "libSceAudioOut", sceAudioOutStartSharePlay);
    LIB_FUNCTION("AImiaYFrKdc", "libSceAudioOut", 1, "libSceAudioOut", sceAudioOutStopAuxBroadcast);
    LIB_FUNCTION("teCyKKZPjME", "libSceAudioOut", 1, "libSceAudioOut", sceAudioOutStopSharePlay);
    LIB_FUNCTION("95bdtHdNUic", "libSceAudioOut", 1, "libSceAudioOut", sceAudioOutSuspendResume);
    LIB_FUNCTION("oRJZnXxok-M", "libSceAudioOut", 1, "libSceAudioOut",
                 sceAudioOutSysConfigureOutputMode);
    LIB_FUNCTION("Tf9-yOJwF-A", "libSceAudioOut", 1, "libSceAudioOut",
                 sceAudioOutSysGetHdmiMonitorInfo);
    LIB_FUNCTION("y2-hP-KoTMI", "libSceAudioOut", 1, "libSceAudioOut", sceAudioOutSysGetSystemInfo);
    LIB_FUNCTION("YV+bnMvMfYg", "libSceAudioOut", 1, "libSceAudioOut",
                 sceAudioOutSysHdmiMonitorInfoIsSupportedAudioOutMode);
    LIB_FUNCTION("JEHhANREcLs", "libSceAudioOut", 1, "libSceAudioOut", sceAudioOutSystemControlGet);
    LIB_FUNCTION("9CHWVv6r3Dg", "libSceAudioOut", 1, "libSceAudioOut", sceAudioOutSystemControlSet);
    LIB_FUNCTION("Mt7JB3lOyJk", "libSceAudioOutSparkControl", 1, "libSceAudioOut",
                 sceAudioOutSparkControlSetEqCoef);
    LIB_FUNCTION("7UsdDOEvjlk", "libSceDbgAudioOut", 1, "libSceAudioOut",
                 sceAudioOutSetSystemDebugState);
};

} // namespace Libraries::AudioOut
