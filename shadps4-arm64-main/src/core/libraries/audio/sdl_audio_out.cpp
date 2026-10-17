// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <SDL3/SDL_audio.h>
#include <SDL3/SDL_events.h>
#include <SDL3/SDL_hints.h>

#include "common/logging/log.h"
#include "core/emulator_settings.h"
#include "core/libraries/audio/audioout.h"
#include "core/libraries/audio/audioout_backend.h"
#include "core/libraries/audio/surround_virtualizer.h"
#include "core/libraries/kernel/threads.h"
#include "core/vr/vr_runtime.h"
#ifdef ENABLE_OPENXR_HOST
#include "core/vr/openxr_host.h"
#endif

// SIMD support detection
#if defined(__x86_64__) || defined(_M_X64)
#include <immintrin.h>
#define HAS_SSE2
#endif

#define SDL_INVALID_AUDIODEVICEID 0

namespace Libraries::AudioOut {

// Volume constants
constexpr float VOLUME_0DB = 32768.0f; // 1 << 15
constexpr float INV_VOLUME_0DB = 1.0f / VOLUME_0DB;
constexpr float VOLUME_EPSILON = 0.001f;
// Timing constants
constexpr u64 VOLUME_CHECK_INTERVAL_US = 50000; // Check every 50ms
constexpr u64 MIN_SLEEP_THRESHOLD_US = 10;
constexpr u64 TIMING_RESYNC_THRESHOLD_US = 100000; // Resync if >100ms behind
constexpr u64 DEVICE_CHECK_INTERVAL_US = 1000000;  // Look for the device once a second
constexpr u64 DEVICE_RETRY_INTERVAL_US = 5000000;  // Try one that would not open again after 5
// How long a change of device must have stood before the port follows it. While the system
// changes its default device (which it does when a device comes or goes), SDL moves the
// streams on the default by itself, and opening or closing a device in the middle of that
// can hang both for good.
constexpr u64 DEVICE_SETTLE_US = 3000000;

// Queue management
constexpr u32 QUEUE_MULTIPLIER = 4;
// Memory alignment for SIMD
constexpr size_t AUDIO_BUFFER_ALIGNMENT = 32;

// Channel positions
enum ChannelPos : u8 {
    FL = 0,
    FR = 1,
    FC = 2,
    LF = 3,
    SL = 4,
    SR = 5,
    BL = 6,
    BR = 7,
    STD_SL = 6,
    STD_SR = 7,
    STD_BL = 4,
    STD_BR = 5
};

/// Says in the log which sound devices come and go while the title runs.
static bool SDLCALL LogDeviceEvent(void*, SDL_Event* event) {
    if (event->type == SDL_EVENT_AUDIO_DEVICE_ADDED ||
        event->type == SDL_EVENT_AUDIO_DEVICE_REMOVED) {
        const SDL_AudioDeviceID id = event->adevice.which;
        const char* name = SDL_GetAudioDeviceName(id);
        LOG_INFO(Lib_AudioOut, "Audio {} {}: {} ({})",
                 event->adevice.recording ? "input" : "output",
                 event->type == SDL_EVENT_AUDIO_DEVICE_ADDED ? "added" : "removed",
                 name ? name : "?", id);
#ifdef ENABLE_OPENXR_HOST
        // Which device is the headset's may have changed with that: a runtime that streams to
        // a headset has its devices only while it does.
        Core::Vr::OpenXrHost::Instance().AudioDevicesChanged();
#endif
    }
    return true;
}

/// The device the system plays on by default, as the device it is. Never "the default device"
/// itself: ports opened on that are moved by SDL when the system's choice changes, and SDL
/// (3.5.0) leaves the list it moves them into in a state in which closing one of them later
/// frees memory its playback thread still reads (SDL_DefaultAudioDeviceChanged does not set
/// the back link of the port it puts in front): the emulator stopped with an access violation
/// in SDL_GetAudioStreamDataAdjustGain a moment after a port left such a device. A port
/// opened on a device of its own stays where it is, and FollowDevice moves it.
static SDL_AudioDeviceID DefaultPlaybackDevice() {
    const char* default_name = SDL_GetAudioDeviceName(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK);
    int count = 0;
    SDL_AudioDeviceID* devices = SDL_GetAudioPlaybackDevices(&count);
    SDL_AudioDeviceID found = SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK;
    for (int i = 0; default_name != nullptr && devices != nullptr && i < count; ++i) {
        const char* name = SDL_GetAudioDeviceName(devices[i]);
        if (name != nullptr && std::strcmp(name, default_name) == 0) {
            found = devices[i];
            break;
        }
    }
    SDL_free(devices);
    return found;
}

class SDLPortBackend : public PortBackend {
public:
    explicit SDLPortBackend(const PortOut& port)
        : frame_size(port.format_info.FrameSize()), guest_buffer_size(port.BufferSize()),
          buffer_frames(port.buffer_frames), sample_rate(port.sample_rate),
          num_channels(port.format_info.num_channels), is_float(port.format_info.is_float),
          is_std(port.format_info.is_std), channel_layout(port.format_info.channel_layout),
          port_type(port.type) {
        // With a headset on, what plays the sound is a pair of speakers at the ears: surround
        // sound is rendered for that (a title mixes it for where the head points) instead of
        // being folded down to left and right. SHADPS4_VIRTUAL_SURROUND=0 turns it off.
        const char* surround = std::getenv("SHADPS4_VIRTUAL_SURROUND");
        if (num_channels == 8 && Core::Vr::Runtime::Instance().IsHeadsetConnected() &&
            (surround == nullptr || surround[0] != '0')) {
            virtualizer.emplace(channel_layout, sample_rate);
            output_channels = 2;
        }

        static std::once_flag watching;
        std::call_once(watching, [] { SDL_AddEventWatch(LogDeviceEvent, nullptr); });

        if (!Initialize()) {
            LOG_ERROR(Lib_AudioOut, "Failed to initialize SDL audio backend");
        }
    }

    ~SDLPortBackend() override {
        Cleanup();
    }

    void Output(void* ptr) override {
        if (!internal_buffer || !convert) [[unlikely]] {
            return;
        }

        const u64 current_time = Kernel::sceKernelGetProcessTime();
        FollowDevice(current_time);
        if (!stream || ptr == nullptr) [[unlikely]] {
            return;
        }

        UpdateVolumeIfChanged();
        if (virtualizer) {
            auto* const stereo = static_cast<float*>(internal_buffer);
            if (is_float) {
                virtualizer->Process(static_cast<const float*>(ptr), buffer_frames, stereo);
            } else {
                virtualizer->Process(static_cast<const s16*>(ptr), buffer_frames, stereo);
            }
        } else {
            convert(ptr, internal_buffer, buffer_frames, nullptr);
        }
        HandleTiming(current_time);

        if ((output_count++ & 0xF) == 0) { // Check every 16 outputs
            ManageAudioQueue();
        }

        if (!SDL_PutAudioStreamData(stream, internal_buffer, internal_buffer_size)) [[unlikely]] {
            LOG_ERROR(Lib_AudioOut, "Failed to output to SDL audio stream: {}", SDL_GetError());
        }

        last_output_time.store(current_time, std::memory_order_release);
    }

    void SetVolume(const std::array<int, 8>& ch_volumes) override {
        std::scoped_lock lock{stream_mutex};
        if (!stream) [[unlikely]] {
            return;
        }

        float max_channel_gain = 0.0f;
        const u32 channels_to_check = std::min(num_channels, 8u);

        for (u32 i = 0; i < channels_to_check; i++) {
            const float channel_gain = static_cast<float>(ch_volumes[i]) * INV_VOLUME_0DB;
            max_channel_gain = std::max(max_channel_gain, channel_gain);
        }

        const float slider_gain = EmulatorSettings.GetVolumeSlider() * 0.01f; // Faster than /100.0f
        const float total_gain = max_channel_gain * slider_gain;

        const float current = current_gain.load(std::memory_order_acquire);
        if (std::abs(total_gain - current) < VOLUME_EPSILON) {
            return;
        }

        // Apply volume change
        if (SDL_SetAudioStreamGain(stream, total_gain)) {
            current_gain.store(total_gain, std::memory_order_release);
            LOG_DEBUG(Lib_AudioOut,
                      "Set combined audio gain to {:.3f} (channel: {:.3f}, slider: {:.3f})",
                      total_gain, max_channel_gain, slider_gain);
        } else {
            LOG_ERROR(Lib_AudioOut, "Failed to set audio stream gain: {}", SDL_GetError());
        }
    }

    u64 GetLastOutputTime() const {
        return last_output_time.load(std::memory_order_acquire);
    }

private:
    bool Initialize() {
        // Calculate timing parameters
        period_us = (1000000ULL * buffer_frames + sample_rate / 2) / sample_rate;

        // Allocate aligned internal buffer for SIMD operations
        internal_buffer_size = buffer_frames * sizeof(float) * output_channels;

#ifdef _WIN32
        internal_buffer = _aligned_malloc(internal_buffer_size, AUDIO_BUFFER_ALIGNMENT);
#else
        if (posix_memalign(&internal_buffer, AUDIO_BUFFER_ALIGNMENT, internal_buffer_size) != 0) {
            internal_buffer = nullptr;
        }
#endif

        if (!internal_buffer) {
            LOG_ERROR(Lib_AudioOut, "Failed to allocate aligned audio buffer of size {}",
                      internal_buffer_size);
            return false;
        }

        // Initialize current gain
        current_gain.store(EmulatorSettings.GetVolumeSlider() * 0.01f, std::memory_order_relaxed);

        if (!SelectConverter()) {
            FreeAlignedBuffer();
            return false;
        }

        // Open SDL device
        if (!OpenDevice()) {
            FreeAlignedBuffer();
            return false;
        }

        CalculateQueueThreshold();
        return true;
    }

    /// Keeps the port on the device it is meant for. One that goes away (a headset's is taken
    /// out of the system whenever the headset stops being streamed to, a USB one is unplugged)
    /// leaves its stream playing into nothing for good, and comes back as another device: the
    /// port moves to the system's default meanwhile, and back once the device is there again.
    void FollowDevice(u64 current_time) {
        const u64 interval = stream ? DEVICE_CHECK_INTERVAL_US : DEVICE_RETRY_INTERVAL_US;
        if (current_time - last_device_check_time < interval) {
            return;
        }
        last_device_check_time = current_time;

        const std::string device_name = GetDeviceName();
        if (device_name == "None") {
            return;
        }
        const Choice choice = SelectAudioDevice(device_name, true);
        const SDL_AudioDeviceID dev_id = choice.device;
        if (stream && dev_id == opened_device) {
            pending_device = SDL_INVALID_AUDIODEVICEID;
            return;
        }
        if (dev_id != pending_device) {
            pending_device = dev_id;
            pending_since = current_time;
            return;
        }
        if (current_time - pending_since < DEVICE_SETTLE_US) {
            return;
        }
        pending_device = SDL_INVALID_AUDIODEVICEID;

        // One port at a time: they all see the change in the same second.
        static std::mutex moving;
        std::scoped_lock one{moving};
        if (stream) {
            const char* name = SDL_GetAudioDeviceName(dev_id);
            if (choice.stands_in) {
                LOG_WARNING(Lib_AudioOut,
                            "Audio device '{}' went away: the sound moves to the system's "
                            "default, '{}'",
                            device_name, name ? name : "?");
            } else if (choice.systems) {
                LOG_INFO(Lib_AudioOut, "The system now plays on '{}': the sound moves to it",
                         name ? name : "?");
            } else {
                LOG_INFO(Lib_AudioOut, "Audio device '{}' is there: the sound moves to it",
                         device_name);
            }
        }
        std::scoped_lock lock{stream_mutex};
        if (stream) {
            SDL_DestroyAudioStream(stream);
            stream = nullptr;
        }
        if (OpenDevice()) {
            CalculateQueueThreshold();
            next_output_time = 0;
        }
    }

    void Cleanup() {
        if (stream) {
            SDL_DestroyAudioStream(stream);
            stream = nullptr;
        }
        FreeAlignedBuffer();
    }

    void FreeAlignedBuffer() {
        if (internal_buffer) {
#ifdef _WIN32
            _aligned_free(internal_buffer);
#else
            free(internal_buffer);
#endif
            internal_buffer = nullptr;
        }
    }

    void UpdateVolumeIfChanged() {
        const u64 current_time = Kernel::sceKernelGetProcessTime();

        if (current_time - last_volume_check_time < VOLUME_CHECK_INTERVAL_US) {
            return;
        }

        last_volume_check_time = current_time;

        const float config_volume = EmulatorSettings.GetVolumeSlider() * 0.01f;
        const float stored_gain = current_gain.load(std::memory_order_acquire);

        // Only update if the difference is significant
        if (std::abs(config_volume - stored_gain) > VOLUME_EPSILON) {
            if (SDL_SetAudioStreamGain(stream, config_volume)) {
                current_gain.store(config_volume, std::memory_order_release);
                LOG_DEBUG(Lib_AudioOut, "Updated audio gain to {:.3f}", config_volume);
            } else {
                LOG_ERROR(Lib_AudioOut, "Failed to set audio stream gain: {}", SDL_GetError());
            }
        }
    }

    void HandleTiming(u64 current_time) {
        if (next_output_time == 0) [[unlikely]] {
            // First output - set initial timing
            next_output_time = current_time + period_us;
            return;
        }

        const s64 time_diff = static_cast<s64>(current_time - next_output_time);

        if (time_diff > static_cast<s64>(TIMING_RESYNC_THRESHOLD_US)) [[unlikely]] {
            // We're far behind - resync
            next_output_time = current_time + period_us;
        } else if (time_diff < 0) {
            // We're ahead of schedule - wait
            const u64 time_to_wait = static_cast<u64>(-time_diff);
            next_output_time += period_us;

            if (time_to_wait > MIN_SLEEP_THRESHOLD_US) {
                // Sleep for most of the wait period
                const u64 sleep_duration = time_to_wait - MIN_SLEEP_THRESHOLD_US;
                std::this_thread::sleep_for(std::chrono::microseconds(sleep_duration));
            }
        } else {
            // Slightly behind or on time - just advance
            next_output_time += period_us;
        }
    }

    void ManageAudioQueue() {
        const auto queued = SDL_GetAudioStreamQueued(stream);

        if (queued >= queue_threshold) [[unlikely]] {
            LOG_DEBUG(Lib_AudioOut, "Clearing backed up audio queue ({} >= {})", queued,
                      queue_threshold);
            SDL_ClearAudioStream(stream);
            CalculateQueueThreshold();
        }
    }

    bool OpenDevice() {
        const SDL_AudioSpec fmt = {
            .format = SDL_AUDIO_F32LE,
            .channels = static_cast<u8>(output_channels),
            .freq = static_cast<int>(sample_rate),
        };

        // Determine device
        const std::string device_name = GetDeviceName();
        const Choice choice = SelectAudioDevice(device_name, false);
        const SDL_AudioDeviceID dev_id = choice.device;

        if (dev_id == SDL_INVALID_AUDIODEVICEID) {
            return false;
        }
        opened_device = dev_id;

        // Create audio stream
        stream = SDL_OpenAudioDeviceStream(dev_id, &fmt, nullptr, nullptr);
        if (!stream) {
            LOG_ERROR(Lib_AudioOut, "Failed to create SDL audio stream: {}", SDL_GetError());
            return false;
        }

        // Configure channel mapping
        if (!ConfigureChannelMap()) {
            SDL_DestroyAudioStream(stream);
            stream = nullptr;
            return false;
        }

        // Set initial volume
        const float initial_gain = current_gain.load(std::memory_order_relaxed);
        if (!SDL_SetAudioStreamGain(stream, initial_gain)) {
            LOG_WARNING(Lib_AudioOut, "Failed to set initial audio gain: {}", SDL_GetError());
        }

        // Start playback
        if (!SDL_ResumeAudioStreamDevice(stream)) {
            LOG_ERROR(Lib_AudioOut, "Failed to resume audio stream: {}", SDL_GetError());
            SDL_DestroyAudioStream(stream);
            stream = nullptr;
            return false;
        }

        const char* opened_name = SDL_GetAudioDeviceName(dev_id);
        LOG_INFO(Lib_AudioOut, "Opened audio device: {}{} ({} Hz, {} ch{}, gain: {:.3f})",
                 opened_name ? opened_name : "?",
                 choice.stands_in ? ", the system's default" : "", sample_rate, num_channels,
                 virtualizer ? " rendered for two speakers at the ears" : "", initial_gain);
        return true;
    }

    /// The device a port is to play on. `systems`: it is the one the system plays on by
    /// default, because the port is left to the system; `stands_in`: or because the device the
    /// port is meant for is not there.
    struct Choice {
        SDL_AudioDeviceID device{SDL_INVALID_AUDIODEVICEID};
        bool systems{};
        bool stands_in{};
    };

    /// The device of that name, or the system's default when there is none (said in the log
    /// unless `quiet`).
    Choice SelectAudioDevice(const std::string& device_name, bool quiet) {
        if (device_name == "None") {
            LOG_INFO(Lib_AudioOut, "Audio device disabled for port type {}",
                     static_cast<int>(port_type));
            return {};
        }

        if (device_name.empty() || device_name == "Default Device") {
            return {.device = DefaultPlaybackDevice(), .systems = true};
        }

        // Search for specific device
        int num_devices = 0;
        SDL_AudioDeviceID* dev_array = SDL_GetAudioPlaybackDevices(&num_devices);

        if (!dev_array) {
            if (!quiet) {
                LOG_WARNING(Lib_AudioOut, "No audio devices found, using default");
            }
            return {.device = DefaultPlaybackDevice(), .stands_in = true};
        }

        SDL_AudioDeviceID selected_device = SDL_INVALID_AUDIODEVICEID;

        for (int i = 0; i < num_devices; i++) {
            const char* dev_name = SDL_GetAudioDeviceName(dev_array[i]);
            if (dev_name && device_name == dev_name) {
                selected_device = dev_array[i];
                break;
            }
        }

        SDL_free(dev_array);

        if (selected_device == SDL_INVALID_AUDIODEVICEID) {
            if (!quiet) {
                LOG_WARNING(Lib_AudioOut, "Audio device '{}' not found, using default",
                            device_name);
            }
            return {.device = DefaultPlaybackDevice(), .stands_in = true};
        }
        return {.device = selected_device};
    }

    bool ConfigureChannelMap() {
        if (num_channels == 0 || virtualizer) {
            // (Left and right, as they come out of the surround stage.)
            return true;
        }

        std::vector<int> channel_map(num_channels);

        if (is_std && num_channels == 8) {
            // Standard 8CH layout requires remapping
            channel_map = {FL, FR, FC, LF, STD_SL, STD_SR, STD_BL, STD_BR};
        } else {
            std::copy_n(channel_layout.begin(), num_channels, channel_map.begin());
        }

        if (!SDL_SetAudioStreamInputChannelMap(stream, channel_map.data(), num_channels)) {
            LOG_ERROR(Lib_AudioOut, "Failed to set channel map: {}", SDL_GetError());
            return false;
        }

        return true;
    }

    std::string GetDeviceName() const {
        std::string name;
        switch (port_type) {
        case OrbisAudioOutPort::Main:
        case OrbisAudioOutPort::Bgm:
            name = EmulatorSettings.GetSDLMainOutputDevice();
            break;
        case OrbisAudioOutPort::PadSpk:
            name = EmulatorSettings.GetSDLPadSpkOutputDevice();
            break;
        default:
            name = EmulatorSettings.GetSDLMainOutputDevice();
            break;
        }
#ifdef ENABLE_OPENXR_HOST
        // Left to the system, the sound goes where the headset of this machine plays it, if
        // its runtime names a device for that (one that streams to a headset has its own).
        if (name.empty() || name == "Default Device") {
            if (const std::string headset = Core::Vr::OpenXrHost::Instance().AudioOutputName();
                !headset.empty()) {
                return headset;
            }
        }
#endif
        return name;
    }

    bool SelectConverter() {
        if (is_float) {
            switch (num_channels) {
            case 1:
                convert = &ConvertF32Mono;
                break;
            case 2:
                convert = &ConvertF32Stereo;
                break;
            case 8:
                convert = is_std ? &ConvertF32Std8CH : &ConvertF32_8CH;
                break;
            default:
                LOG_ERROR(Lib_AudioOut, "Unsupported float channel count: {}", num_channels);
                return false;
            }
        } else {
            switch (num_channels) {
            case 1:
                convert = &ConvertS16Mono;
                break;
            case 2:
#if defined(HAS_SSE2)
                convert = &ConvertS16StereoSIMD;
#else
                convert = &ConvertS16Stereo;
#endif
                break;
            case 8:
#if defined(HAS_SSE2)
                convert = &ConvertS16_8CH_SIMD;
#else
                convert = &ConvertS16_8CH;
#endif
                break;
            default:
                LOG_ERROR(Lib_AudioOut, "Unsupported S16 channel count: {}", num_channels);
                return false;
            }
        }

        return true;
    }

    void CalculateQueueThreshold() {
        if (!stream) {
            return;
        }

        SDL_AudioSpec discard;
        int sdl_buffer_frames = 0;

        if (!SDL_GetAudioDeviceFormat(SDL_GetAudioStreamDevice(stream), &discard,
                                      &sdl_buffer_frames)) {
            LOG_WARNING(Lib_AudioOut, "Failed to get SDL buffer size: {}", SDL_GetError());
        }

        // What is queued is what comes out of the conversion: as many channels as go out.
        const u32 sdl_buffer_size = sdl_buffer_frames * sizeof(float) * output_channels;
        queue_threshold =
            std::max(buffer_frames * static_cast<u32>(sizeof(float)) * output_channels,
                     sdl_buffer_size) *
            QUEUE_MULTIPLIER;

        LOG_DEBUG(Lib_AudioOut, "Audio queue threshold: {} bytes (SDL buffer: {} frames)",
                  queue_threshold, sdl_buffer_frames);
    }

    using ConverterFunc = void (*)(const void* src, void* dst, u32 frames, const float* volumes);

    static void ConvertS16Mono(const void* src, void* dst, u32 frames, const float*) {
        const s16* s = static_cast<const s16*>(src);
        float* d = static_cast<float*>(dst);

        for (u32 i = 0; i < frames; i++) {
            d[i] = s[i] * INV_VOLUME_0DB;
        }
    }

    static void ConvertS16Stereo(const void* src, void* dst, u32 frames, const float*) {
        const s16* s = static_cast<const s16*>(src);
        float* d = static_cast<float*>(dst);

        const u32 num_samples = frames << 1; // * 2
        for (u32 i = 0; i < num_samples; i++) {
            d[i] = s[i] * INV_VOLUME_0DB;
        }
    }

#ifdef HAS_SSE2
    static void ConvertS16StereoSIMD(const void* src, void* dst, u32 frames, const float*) {
        const s16* s = static_cast<const s16*>(src);
        float* d = static_cast<float*>(dst);

        const __m128 scale = _mm_set1_ps(INV_VOLUME_0DB);
        const u32 num_samples = frames << 1;
        u32 i = 0;

        // Process 8 samples at a time (4 stereo frames)
        for (; i + 8 <= num_samples; i += 8) {
            // Load 8 s16 values
            __m128i s16_vals = _mm_loadu_si128(reinterpret_cast<const __m128i*>(&s[i]));

            // Convert to 32-bit integers
            __m128i s32_lo = _mm_cvtepi16_epi32(s16_vals);
            __m128i s32_hi = _mm_cvtepi16_epi32(_mm_srli_si128(s16_vals, 8));

            // Convert to float and scale
            __m128 f_lo = _mm_mul_ps(_mm_cvtepi32_ps(s32_lo), scale);
            __m128 f_hi = _mm_mul_ps(_mm_cvtepi32_ps(s32_hi), scale);

            // Store results
            _mm_storeu_ps(&d[i], f_lo);
            _mm_storeu_ps(&d[i + 4], f_hi);
        }

        // Handle remaining samples
        for (; i < num_samples; i++) {
            d[i] = s[i] * INV_VOLUME_0DB;
        }
    }
#endif

    static void ConvertS16_8CH(const void* src, void* dst, u32 frames, const float*) {
        const s16* s = static_cast<const s16*>(src);
        float* d = static_cast<float*>(dst);

        const u32 num_samples = frames << 3; // * 8
        for (u32 i = 0; i < num_samples; i++) {
            d[i] = s[i] * INV_VOLUME_0DB;
        }
    }

#ifdef HAS_SSE2
    static void ConvertS16_8CH_SIMD(const void* src, void* dst, u32 frames, const float*) {
        const s16* s = static_cast<const s16*>(src);
        float* d = static_cast<float*>(dst);

        const __m128 scale = _mm_set1_ps(INV_VOLUME_0DB);
        const u32 num_samples = frames << 3;
        u32 i = 0;

        // Process 8 samples at a time
        for (; i + 8 <= num_samples; i += 8) {
            __m128i s16_vals = _mm_loadu_si128(reinterpret_cast<const __m128i*>(&s[i]));
            __m128i s32_lo = _mm_cvtepi16_epi32(s16_vals);
            __m128i s32_hi = _mm_cvtepi16_epi32(_mm_srli_si128(s16_vals, 8));
            __m128 f_lo = _mm_mul_ps(_mm_cvtepi32_ps(s32_lo), scale);
            __m128 f_hi = _mm_mul_ps(_mm_cvtepi32_ps(s32_hi), scale);
            _mm_storeu_ps(&d[i], f_lo);
            _mm_storeu_ps(&d[i + 4], f_hi);
        }

        for (; i < num_samples; i++) {
            d[i] = s[i] * INV_VOLUME_0DB;
        }
    }
#endif

    static void ConvertF32Mono(const void* src, void* dst, u32 frames, const float*) {
        std::memcpy(dst, src, frames * sizeof(float));
    }

    static void ConvertF32Stereo(const void* src, void* dst, u32 frames, const float*) {
        std::memcpy(dst, src, frames * 2 * sizeof(float));
    }

    static void ConvertF32_8CH(const void* src, void* dst, u32 frames, const float*) {
        std::memcpy(dst, src, frames * 8 * sizeof(float));
    }

    static void ConvertF32Std8CH(const void* src, void* dst, u32 frames, const float*) {
        const float* s = static_cast<const float*>(src);
        float* d = static_cast<float*>(dst);

        // Channel remapping for standard 8CH layout
        for (u32 i = 0; i < frames; i++) {
            const u32 offset = i << 3; // * 8

            d[offset + FL] = s[offset + FL];
            d[offset + FR] = s[offset + FR];
            d[offset + FC] = s[offset + FC];
            d[offset + LF] = s[offset + LF];
            d[offset + SL] = s[offset + STD_SL];
            d[offset + SR] = s[offset + STD_SR];
            d[offset + BL] = s[offset + STD_BL];
            d[offset + BR] = s[offset + STD_BR];
        }
    }

    // Audio format parameters
    const u32 frame_size;
    const u32 guest_buffer_size;
    const u32 buffer_frames;
    const u32 sample_rate;
    const u32 num_channels;
    const bool is_float;
    const bool is_std;
    const std::array<int, 8> channel_layout;
    const OrbisAudioOutPort port_type;
    /// Channels handed to the device: the port's own, or two where surround sound is
    /// rendered for a pair of speakers at the ears.
    u32 output_channels{num_channels};
    std::optional<SurroundVirtualizer> virtualizer;

    alignas(64) u64 period_us{0};
    alignas(64) std::atomic<u64> last_output_time{0};
    u64 next_output_time{0};
    u64 last_volume_check_time{0};
    u32 output_count{0};

    // Buffers
    u32 internal_buffer_size{0};
    void* internal_buffer{nullptr};

    // Converter function pointer
    ConverterFunc convert{nullptr};

    // Volume management
    alignas(64) std::atomic<float> current_gain{1.0f};

    // SDL audio stream. Made, and remade when its device changes, by the thread that plays;
    // the mutex is for the others (the volume).
    std::mutex stream_mutex;
    SDL_AudioStream* stream{nullptr};
    /// What the stream was opened on: a device of the system, or its default.
    SDL_AudioDeviceID opened_device{SDL_INVALID_AUDIODEVICEID};
    u64 last_device_check_time{0};
    /// The device the port is to move to, once it has been the one for long enough.
    SDL_AudioDeviceID pending_device{SDL_INVALID_AUDIODEVICEID};
    u64 pending_since{0};
    u32 queue_threshold{0};
};

std::unique_ptr<PortBackend> SDLAudioOut::Open(PortOut& port) {
    return std::make_unique<SDLPortBackend>(port);
}

} // namespace Libraries::AudioOut
