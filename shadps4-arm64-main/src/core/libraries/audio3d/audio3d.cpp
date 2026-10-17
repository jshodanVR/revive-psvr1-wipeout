// SPDX-FileCopyrightText: Copyright 2025-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <magic_enum/magic_enum.hpp>

#include "common/assert.h"
#include "common/logging/log.h"
#include "core/libraries/audio/audioout.h"
#include "core/libraries/audio/audioout_error.h"
#include "core/libraries/audio3d/audio3d.h"
#include "core/libraries/audio3d/audio3d_error.h"
#include "core/libraries/error_codes.h"
#include "core/libraries/libs.h"

namespace Libraries::Audio3d {

static constexpr u32 AUDIO3D_SAMPLE_RATE = 48000;

static constexpr AudioOut::OrbisAudioOutParamFormat AUDIO3D_OUTPUT_FORMAT =
    AudioOut::OrbisAudioOutParamFormat::S16Stereo;
static constexpr u32 AUDIO3D_OUTPUT_NUM_CHANNELS = 2;

static std::unique_ptr<Audio3dState> state;

/// SHADPS4_AUDIO3D_TRACE=1 logs, a few times a second, where the loudest objects are.
static bool TraceEnabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("SHADPS4_AUDIO3D_TRACE");
        return value != nullptr && *value != '\0' && *value != '0';
    }();
    return enabled;
}

static void ResetObject(ObjectState& obj) {
    obj.pcm.clear();
    obj.has_pcm = false;
    obj.placement = {};
    obj.priority = 0;
    obj.passthrough = false;
    obj.ambisonic_channel = -1;
    obj.spatializer.Reset();
    obj.persistent_attributes.clear();
}

/// Applies one attribute other than a reset to an object.
static s32 ApplyAttribute(const Port& port, ObjectState& obj, const OrbisAudio3dAttributeId id,
                          const void* value, const u64 size) {
    const auto read = [&](auto& out) {
        if (value != nullptr && size >= sizeof(out)) {
            std::memcpy(&out, value, sizeof(out));
            return true;
        }
        return false;
    };

    switch (id) {
    case OrbisAudio3dAttributeId::ORBIS_AUDIO3D_ATTRIBUTE_PCM: {
        OrbisAudio3dPcm pcm{};
        if (!read(pcm)) {
            LOG_ERROR(Lib_Audio3d, "PCM attribute value_size too small");
            return ORBIS_AUDIO3D_ERROR_INVALID_PARAMETER;
        }
        if (!pcm.sample_buffer || !pcm.num_samples) {
            return ORBIS_AUDIO3D_ERROR_INVALID_PARAMETER;
        }
        // An object carries one channel, one block at a time: what is set replaces what was
        // not played yet. A short block is padded with silence.
        const u32 granularity = port.parameters.granularity;
        const u32 count = std::min(pcm.num_samples, granularity);
        obj.pcm.assign(granularity, 0.0f);
        if (pcm.format == OrbisAudio3dFormat::ORBIS_AUDIO3D_FORMAT_S16) {
            const auto* source = static_cast<const s16*>(pcm.sample_buffer);
            for (u32 i = 0; i < count; ++i) {
                obj.pcm[i] = source[i] / 32768.0f;
            }
        } else {
            std::memcpy(obj.pcm.data(), pcm.sample_buffer, count * sizeof(float));
        }
        obj.has_pcm = true;
        return ORBIS_OK;
    }
    case OrbisAudio3dAttributeId::ORBIS_AUDIO3D_ATTRIBUTE_POSITION: {
        float position[3];
        if (read(position)) {
            obj.placement.x = position[0];
            obj.placement.y = position[1];
            obj.placement.z = position[2];
        }
        return ORBIS_OK;
    }
    case OrbisAudio3dAttributeId::ORBIS_AUDIO3D_ATTRIBUTE_GAIN:
        read(obj.placement.gain);
        return ORBIS_OK;
    case OrbisAudio3dAttributeId::ORBIS_AUDIO3D_ATTRIBUTE_SPREAD:
        read(obj.placement.spread);
        return ORBIS_OK;
    case OrbisAudio3dAttributeId::ORBIS_AUDIO3D_ATTRIBUTE_PRIORITY:
        read(obj.priority);
        return ORBIS_OK;
    case OrbisAudio3dAttributeId::ORBIS_AUDIO3D_ATTRIBUTE_AMBISONICS: {
        u32 channel = 0;
        if (read(channel)) {
            obj.ambisonic_channel = static_cast<s32>(channel);
        }
        return ORBIS_OK;
    }
    case OrbisAudio3dAttributeId::ORBIS_AUDIO3D_ATTRIBUTE_PASSTHROUGH: {
        u32 enabled = 0;
        if (read(enabled)) {
            obj.passthrough = enabled != 0;
        }
        return ORBIS_OK;
    }
    default:
        if (value != nullptr && size > 0 && size <= 4096) {
            const auto* src = static_cast<const u8*>(value);
            obj.persistent_attributes[static_cast<u32>(id)].assign(src, src + size);
        }
        return ORBIS_OK;
    }
}

s32 PS4_SYSV_ABI sceAudio3dAudioOutClose(const s32 handle) {
    LOG_INFO(Lib_Audio3d, "called, handle = {}", handle);

    // Remove from any port that was tracking this handle.
    if (state) {
        for (auto& [port_id, port] : state->ports) {
            std::scoped_lock lock{port.mutex};
            auto& handles = port.audioout_handles;
            handles.erase(std::remove(handles.begin(), handles.end(), handle), handles.end());
        }
    }

    return AudioOut::sceAudioOutClose(handle);
}

s32 PS4_SYSV_ABI sceAudio3dAudioOutOpen(
    const OrbisAudio3dPortId port_id, const Libraries::UserService::OrbisUserServiceUserId user_id,
    s32 type, const s32 index, const u32 len, const u32 freq,
    const AudioOut::OrbisAudioOutParamExtendedInformation param) {
    LOG_INFO(Lib_Audio3d,
             "called, port_id = {}, user_id = {}, type = {}, index = {}, len = {}, freq = {}",
             port_id, user_id, type, index, len, freq);

    if (!state->ports.contains(port_id)) {
        LOG_ERROR(Lib_Audio3d, "!state->ports.contains(port_id)");
        return ORBIS_AUDIO3D_ERROR_INVALID_PORT;
    }

    std::scoped_lock lock{state->ports[port_id].mutex};
    if (len != state->ports[port_id].parameters.granularity) {
        LOG_ERROR(Lib_Audio3d, "len != state->ports[port_id].parameters.granularity");
        return ORBIS_AUDIO3D_ERROR_INVALID_PARAMETER;
    }

    const s32 handle = sceAudioOutOpen(user_id, static_cast<AudioOut::OrbisAudioOutPort>(type),
                                       index, len, freq, param);
    if (handle < 0) {
        return handle;
    }

    // Track this handle in the port so sceAudio3dPortFlush can use it for sync.
    state->ports[port_id].audioout_handles.push_back(handle);
    return handle;
}

s32 PS4_SYSV_ABI sceAudio3dAudioOutOutput(const s32 handle, void* ptr) {
    LOG_DEBUG(Lib_Audio3d, "called, handle = {}, ptr = {}", handle, ptr);

    if (!ptr) {
        LOG_ERROR(Lib_Audio3d, "!ptr");
        return ORBIS_AUDIO3D_ERROR_INVALID_PARAMETER;
    }

    if (handle < 0 || (handle & 0xFFFF) > 25) {
        LOG_ERROR(Lib_Audio3d, "handle < 0 || (handle & 0xFFFF) > 25");
        return ORBIS_AUDIO3D_ERROR_INVALID_PORT;
    }

    return AudioOut::sceAudioOutOutput(handle, ptr);
}

s32 PS4_SYSV_ABI sceAudio3dAudioOutOutputs(AudioOut::OrbisAudioOutOutputParam* param,
                                           const u32 num) {
    LOG_DEBUG(Lib_Audio3d, "called, param = {}, num = {}", static_cast<void*>(param), num);

    if (!param || !num) {
        LOG_ERROR(Lib_Audio3d, "!param || !num");
        return ORBIS_AUDIO3D_ERROR_INVALID_PARAMETER;
    }

    return AudioOut::sceAudioOutOutputs(param, num);
}

static s32 ConvertAndEnqueue(std::deque<AudioData>& queue, const OrbisAudio3dPcm& pcm,
                             const u32 num_channels, const u32 granularity) {
    if (!pcm.sample_buffer || !pcm.num_samples) {
        return ORBIS_AUDIO3D_ERROR_INVALID_PARAMETER;
    }

    const u32 bytes_per_sample =
        (pcm.format == OrbisAudio3dFormat::ORBIS_AUDIO3D_FORMAT_S16) ? sizeof(s16) : sizeof(float);

    // Always allocate exactly granularity samples (zeroed = silence for padding).
    const u32 dst_bytes = granularity * num_channels * bytes_per_sample;
    u8* copy = static_cast<u8*>(std::calloc(1, dst_bytes));
    if (!copy) {
        return ORBIS_AUDIO3D_ERROR_OUT_OF_MEMORY;
    }

    // Copy min(provided, granularity) samples — extra are dropped, shortage stays zero.
    const u32 samples_to_copy = std::min(pcm.num_samples, granularity);
    std::memcpy(copy, pcm.sample_buffer, samples_to_copy * num_channels * bytes_per_sample);

    queue.emplace_back(AudioData{
        .sample_buffer = copy,
        .num_samples = granularity,
        .num_channels = num_channels,
        .format = pcm.format,
    });
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceAudio3dBedWrite(const OrbisAudio3dPortId port_id, const u32 num_channels,
                                    const OrbisAudio3dFormat format, void* buffer,
                                    const u32 num_samples) {
    return sceAudio3dBedWrite2(port_id, num_channels, format, buffer, num_samples,
                               OrbisAudio3dOutputRoute::ORBIS_AUDIO3D_OUTPUT_BOTH, false);
}

s32 PS4_SYSV_ABI sceAudio3dBedWrite2(const OrbisAudio3dPortId port_id, const u32 num_channels,
                                     const OrbisAudio3dFormat format, void* buffer,
                                     const u32 num_samples,
                                     const OrbisAudio3dOutputRoute output_route,
                                     const bool restricted) {
    LOG_DEBUG(
        Lib_Audio3d,
        "called, port_id = {}, num_channels = {}, format = {}, num_samples = {}, output_route "
        "= {}, restricted = {}",
        port_id, num_channels, magic_enum::enum_name(format), num_samples,
        magic_enum::enum_name(output_route), restricted);

    if (!state->ports.contains(port_id)) {
        LOG_ERROR(Lib_Audio3d, "!state->ports.contains(port_id)");
        return ORBIS_AUDIO3D_ERROR_INVALID_PORT;
    }

    // BOTH is zero; headset-only and TV-only are also valid destinations.
    if (output_route > OrbisAudio3dOutputRoute::ORBIS_AUDIO3D_OUTPUT_TV_ONLY) {
        LOG_ERROR(Lib_Audio3d, "Invalid output route: {}", static_cast<u32>(output_route));
        return ORBIS_AUDIO3D_ERROR_INVALID_PARAMETER;
    }

    if (format > OrbisAudio3dFormat::ORBIS_AUDIO3D_FORMAT_FLOAT) {
        LOG_ERROR(Lib_Audio3d, "format > ORBIS_AUDIO3D_FORMAT_FLOAT");
        return ORBIS_AUDIO3D_ERROR_INVALID_PARAMETER;
    }

    if (num_channels != 2 && num_channels != 6 && num_channels != 8) {
        LOG_ERROR(Lib_Audio3d, "num_channels must be 2, 6, or 8");
        return ORBIS_AUDIO3D_ERROR_INVALID_PARAMETER;
    }

    if (!buffer || !num_samples) {
        LOG_ERROR(Lib_Audio3d, "!buffer || !num_samples");
        return ORBIS_AUDIO3D_ERROR_INVALID_PARAMETER;
    }

    if (format == OrbisAudio3dFormat::ORBIS_AUDIO3D_FORMAT_FLOAT) {
        if ((reinterpret_cast<uintptr_t>(buffer) & 3) != 0) {
            LOG_ERROR(Lib_Audio3d, "buffer & 3 != 0");
            return ORBIS_AUDIO3D_ERROR_INVALID_PARAMETER;
        }
    } else if (format == OrbisAudio3dFormat::ORBIS_AUDIO3D_FORMAT_S16) {
        if ((reinterpret_cast<uintptr_t>(buffer) & 1) != 0) {
            LOG_ERROR(Lib_Audio3d, "buffer & 1 != 0");
            return ORBIS_AUDIO3D_ERROR_INVALID_PARAMETER;
        }
    }

    std::scoped_lock lock{state->ports[port_id].mutex};
    return ConvertAndEnqueue(state->ports[port_id].bed_queue,
                             OrbisAudio3dPcm{
                                 .format = format,
                                 .sample_buffer = buffer,
                                 .num_samples = num_samples,
                             },
                             num_channels, state->ports[port_id].parameters.granularity);
}

s32 PS4_SYSV_ABI sceAudio3dCreateSpeakerArray() {
    LOG_ERROR(Lib_Audio3d, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceAudio3dDeleteSpeakerArray() {
    LOG_ERROR(Lib_Audio3d, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceAudio3dGetDefaultOpenParameters(OrbisAudio3dOpenParameters* params) {
    LOG_DEBUG(Lib_Audio3d, "called");
    if (params) {
        auto default_params = OrbisAudio3dOpenParameters{
            .size_this = 0x20,
            .granularity = 0x100,
            .rate = OrbisAudio3dRate::ORBIS_AUDIO3D_RATE_48000,
            .max_objects = 512,
            .queue_depth = 2,
            .buffer_mode = OrbisAudio3dBufferMode::ORBIS_AUDIO3D_BUFFER_ADVANCE_AND_PUSH,
        };
        memcpy(params, &default_params, 0x20);
    }
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceAudio3dGetSpeakerArrayMemorySize() {
    LOG_ERROR(Lib_Audio3d, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceAudio3dGetSpeakerArrayMixCoefficients() {
    LOG_ERROR(Lib_Audio3d, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceAudio3dGetSpeakerArrayMixCoefficients2() {
    LOG_ERROR(Lib_Audio3d, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceAudio3dInitialize(const s64 reserved) {
    LOG_INFO(Lib_Audio3d, "called, reserved = {}", reserved);

    if (reserved != 0) {
        LOG_ERROR(Lib_Audio3d, "reserved != 0");
        return ORBIS_AUDIO3D_ERROR_INVALID_PARAMETER;
    }

    if (state) {
        LOG_ERROR(Lib_Audio3d, "already initialized");
        return ORBIS_AUDIO3D_ERROR_NOT_READY;
    }

    state = std::make_unique<Audio3dState>();

    if (const auto init_ret = AudioOut::sceAudioOutInit();
        init_ret < 0 && init_ret != ORBIS_AUDIO_OUT_ERROR_ALREADY_INIT) {
        return init_ret;
    }

    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceAudio3dObjectReserve(const OrbisAudio3dPortId port_id,
                                         OrbisAudio3dObjectId* object_id) {
    LOG_INFO(Lib_Audio3d, "called, port_id = {}, object_id = {}", port_id,
             static_cast<void*>(object_id));

    if (!object_id) {
        LOG_ERROR(Lib_Audio3d, "!object_id");
        return ORBIS_AUDIO3D_ERROR_INVALID_PARAMETER;
    }

    *object_id = ORBIS_AUDIO3D_OBJECT_INVALID;

    if (!state->ports.contains(port_id)) {
        LOG_ERROR(Lib_Audio3d, "!state->ports.contains(port_id)");
        return ORBIS_AUDIO3D_ERROR_INVALID_PORT;
    }

    auto& port = state->ports[port_id];
    std::scoped_lock lock{port.mutex};

    // Enforce the max_objects limit set at PortOpen time.
    if (port.objects.size() >= port.parameters.max_objects) {
        LOG_ERROR(Lib_Audio3d, "port has no available objects (max_objects = {})",
                  port.parameters.max_objects);
        return ORBIS_AUDIO3D_ERROR_OUT_OF_RESOURCES;
    }

    // counter lives in the Port so it resets when the port is closed and reopened,
    do {
        ++port.next_object_id;
    } while (port.next_object_id == 0 || port.next_object_id == ORBIS_AUDIO3D_OBJECT_INVALID ||
             port.objects.contains(port.next_object_id));

    *object_id = port.next_object_id;
    port.objects.emplace(*object_id, ObjectState{});
    LOG_INFO(Lib_Audio3d, "reserved object_id = {}", *object_id);

    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceAudio3dObjectSetAttribute(const OrbisAudio3dPortId port_id,
                                              const OrbisAudio3dObjectId object_id,
                                              const OrbisAudio3dAttributeId attribute_id,
                                              const void* attribute, const u64 attribute_size) {
    LOG_DEBUG(Lib_Audio3d, "called, port_id = {}, object_id = {}, attribute_id = {:#x}, size = {}",
              port_id, object_id, static_cast<u32>(attribute_id), attribute_size);

    if (!state->ports.contains(port_id)) {
        LOG_ERROR(Lib_Audio3d, "!state->ports.contains(port_id)");
        return ORBIS_AUDIO3D_ERROR_INVALID_PORT;
    }

    auto& port = state->ports[port_id];
    std::scoped_lock lock{port.mutex};
    if (!port.objects.contains(object_id)) {
        LOG_DEBUG(Lib_Audio3d, "object_id {} not reserved (race with Unreserve?), no-op",
                  object_id);
        return ORBIS_OK;
    }

    if (!attribute_size &&
        attribute_id != OrbisAudio3dAttributeId::ORBIS_AUDIO3D_ATTRIBUTE_RESET_STATE) {
        LOG_ERROR(Lib_Audio3d, "!attribute_size for non-reset attribute");
        return ORBIS_AUDIO3D_ERROR_INVALID_PARAMETER;
    }

    auto& obj = port.objects[object_id];

    // RESET_STATE clears all attributes and pending PCM; it takes no value.
    if (attribute_id == OrbisAudio3dAttributeId::ORBIS_AUDIO3D_ATTRIBUTE_RESET_STATE) {
        ResetObject(obj);
        return ORBIS_OK;
    }
    return ApplyAttribute(port, obj, attribute_id, attribute, attribute_size);
}

s32 PS4_SYSV_ABI sceAudio3dObjectSetAttributes(const OrbisAudio3dPortId port_id,
                                               OrbisAudio3dObjectId object_id,
                                               const u64 num_attributes,
                                               const OrbisAudio3dAttribute* attribute_array) {
    LOG_DEBUG(Lib_Audio3d,
              "called, port_id = {}, object_id = {}, num_attributes = {}, attribute_array = {}",
              port_id, object_id, num_attributes, fmt::ptr(attribute_array));

    if (!state->ports.contains(port_id)) {
        LOG_ERROR(Lib_Audio3d, "!state->ports.contains(port_id)");
        return ORBIS_AUDIO3D_ERROR_INVALID_PORT;
    }

    if (!num_attributes || !attribute_array) {
        LOG_ERROR(Lib_Audio3d, "!num_attributes || !attribute_array");
        return ORBIS_AUDIO3D_ERROR_INVALID_PARAMETER;
    }

    auto& port = state->ports[port_id];
    std::scoped_lock lock{port.mutex};
    if (!port.objects.contains(object_id)) {
        LOG_DEBUG(Lib_Audio3d, "object_id {} not reserved", object_id);
        return ORBIS_OK;
    }

    auto& obj = port.objects[object_id];

    // A reset comes first wherever it is listed.
    for (u64 i = 0; i < num_attributes; i++) {
        if (attribute_array[i].attribute_id ==
            OrbisAudio3dAttributeId::ORBIS_AUDIO3D_ATTRIBUTE_RESET_STATE) {
            ResetObject(obj);
            break;
        }
    }

    for (u64 i = 0; i < num_attributes; i++) {
        const auto& attribute = attribute_array[i];
        if (attribute.attribute_id ==
            OrbisAudio3dAttributeId::ORBIS_AUDIO3D_ATTRIBUTE_RESET_STATE) {
            continue;
        }
        if (const s32 ret = ApplyAttribute(port, obj, attribute.attribute_id, attribute.value,
                                           attribute.value_size);
            ret != ORBIS_OK) {
            return ret;
        }
    }

    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceAudio3dObjectUnreserve(const OrbisAudio3dPortId port_id,
                                           const OrbisAudio3dObjectId object_id) {
    LOG_DEBUG(Lib_Audio3d, "called, port_id = {}, object_id = {}", port_id, object_id);

    if (!state->ports.contains(port_id)) {
        LOG_ERROR(Lib_Audio3d, "!state->ports.contains(port_id)");
        return ORBIS_AUDIO3D_ERROR_INVALID_PORT;
    }

    auto& port = state->ports[port_id];
    std::scoped_lock lock{port.mutex};

    if (!port.objects.contains(object_id)) {
        LOG_ERROR(Lib_Audio3d, "object_id not reserved");
        return ORBIS_AUDIO3D_ERROR_INVALID_OBJECT;
    }

    port.objects.erase(object_id);
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceAudio3dPortAdvance(const OrbisAudio3dPortId port_id) {
    LOG_DEBUG(Lib_Audio3d, "called, port_id = {}", port_id);

    if (!state->ports.contains(port_id)) {
        LOG_ERROR(Lib_Audio3d, "!state->ports.contains(port_id)");
        return ORBIS_AUDIO3D_ERROR_INVALID_PORT;
    }

    auto& port = state->ports[port_id];
    std::scoped_lock lock{port.mutex};

    if (port.parameters.buffer_mode == OrbisAudio3dBufferMode::ORBIS_AUDIO3D_BUFFER_NO_ADVANCE) {
        LOG_ERROR(Lib_Audio3d, "port doesn't have advance capability");
        return ORBIS_AUDIO3D_ERROR_NOT_SUPPORTED;
    }

    if (port.mixed_queue.size() >= port.parameters.queue_depth) {
        LOG_WARNING(Lib_Audio3d, "mixed queue full (depth={}), dropping advance",
                    port.parameters.queue_depth);
        return ORBIS_AUDIO3D_ERROR_NOT_READY;
    }

    const u32 granularity = port.parameters.granularity;
    const u32 out_samples = granularity * AUDIO3D_OUTPUT_NUM_CHANNELS;
    port.mix_buffer.assign(out_samples, 0.0f);
    float* mix = port.mix_buffer.data();

    // The bed is ordinary speaker audio and goes in as it is.
    if (!port.bed_queue.empty()) {
        AudioData data = port.bed_queue.front();
        port.bed_queue.pop_front();
        const u32 frames = std::min(granularity, data.num_samples);
        const u32 channels = data.num_channels;
        const bool is_s16 = data.format == OrbisAudio3dFormat::ORBIS_AUDIO3D_FORMAT_S16;
        const auto sample = [&](u32 frame, u32 channel) {
            const size_t at = size_t{frame} * channels + channel;
            return is_s16 ? reinterpret_cast<const s16*>(data.sample_buffer)[at] / 32768.0f
                          : reinterpret_cast<const float*>(data.sample_buffer)[at];
        };
        for (u32 i = 0; i < frames; i++) {
            mix[i * 2 + 0] += sample(i, 0);
            mix[i * 2 + 1] += sample(i, channels > 1 ? 1 : 0);
        }
        std::free(data.sample_buffer);
    }

    // Every object that got a block since the last advance is placed around the listener.
    u32 active = 0;
    const ObjectState* loudest = nullptr;
    for (auto& [obj_id, obj] : port.objects) {
        if (!obj.has_pcm) {
            continue;
        }
        obj.has_pcm = false;
        ++active;
        if (obj.ambisonic_channel > 0) {
            // The channels after the first describe from where the field arrives. Without a
            // decoder for them the field is played through its first channel, which holds all
            // of the sound and none of its direction.
            continue;
        }
        if (obj.passthrough || obj.ambisonic_channel == 0) {
            const float gain = obj.placement.gain;
            for (u32 i = 0; i < granularity; i++) {
                mix[i * 2 + 0] += obj.pcm[i] * gain;
                mix[i * 2 + 1] += obj.pcm[i] * gain;
            }
        } else {
            obj.spatializer.Process(obj.pcm.data(), granularity, mix, obj.placement,
                                    static_cast<float>(AUDIO3D_SAMPLE_RATE));
        }
        if (loudest == nullptr || obj.placement.gain > loudest->placement.gain) {
            loudest = &obj;
        }
    }

    if (TraceEnabled() && (port.advance_count++ % 256) == 0 && loudest != nullptr) {
        LOG_INFO(Lib_Audio3d, "trace: {} objects with sound", active);
        u32 listed = 0;
        for (const auto& [obj_id, obj] : port.objects) {
            if (obj.pcm.empty() || ++listed > 24) {
                continue;
            }
            std::string extra;
            for (const auto& [id, blob] : obj.persistent_attributes) {
                extra += fmt::format(" attr{}[{}]=", id, blob.size());
                for (size_t i = 0; i < blob.size() && i < 16; ++i) {
                    extra += fmt::format("{:02x}", blob[i]);
                }
            }
            float peak = 0.0f;
            for (const float sample : obj.pcm) {
                peak = std::max(peak, std::abs(sample));
            }
            LOG_INFO(Lib_Audio3d,
                     "trace:   #{} pos ({:.2f}, {:.2f}, {:.2f}) gain {:.3f} spread {:.2f} "
                     "priority {} passthrough {} ambisonic {} peak {:.3f}{}",
                     obj_id, obj.placement.x, obj.placement.y, obj.placement.z,
                     obj.placement.gain, obj.placement.spread, obj.priority, obj.passthrough,
                     obj.ambisonic_channel, peak, extra);
        }
    }

    // Many objects at once can add up to more than full scale: turn the whole mix down then,
    // and let it come back slowly.
    s16* mix_s16 = static_cast<s16*>(std::malloc(out_samples * sizeof(s16)));
    if (!mix_s16) {
        return ORBIS_AUDIO3D_ERROR_OUT_OF_MEMORY;
    }
    float gain = port.limiter_gain;
    for (u32 i = 0; i < granularity; i++) {
        const float peak = std::max(std::abs(mix[i * 2]), std::abs(mix[i * 2 + 1]));
        const float target = peak > 1.0f ? 1.0f / peak : 1.0f;
        gain = target < gain ? target : gain + (target - gain) * 0.0002f;
        mix_s16[i * 2 + 0] = static_cast<s16>(std::clamp(mix[i * 2 + 0] * gain, -1.0f, 1.0f) *
                                              32767.0f);
        mix_s16[i * 2 + 1] = static_cast<s16>(std::clamp(mix[i * 2 + 1] * gain, -1.0f, 1.0f) *
                                              32767.0f);
    }
    port.limiter_gain = std::isfinite(gain) ? gain : 1.0f;

    port.mixed_queue.push_back(AudioData{.sample_buffer = reinterpret_cast<u8*>(mix_s16),
                                         .num_samples = granularity,
                                         .num_channels = AUDIO3D_OUTPUT_NUM_CHANNELS,
                                         .format = OrbisAudio3dFormat::ORBIS_AUDIO3D_FORMAT_S16});

    return ORBIS_OK;
}
s32 PS4_SYSV_ABI sceAudio3dPortClose(const OrbisAudio3dPortId port_id) {
    LOG_INFO(Lib_Audio3d, "called, port_id = {}", port_id);

    if (!state->ports.contains(port_id)) {
        LOG_ERROR(Lib_Audio3d, "!state->ports.contains(port_id)");
        return ORBIS_AUDIO3D_ERROR_INVALID_PORT;
    }

    auto& port = state->ports[port_id];
    {
        std::scoped_lock lock{port.mutex};

        if (port.audio_out_handle >= 0) {
            AudioOut::sceAudioOutClose(port.audio_out_handle);
            port.audio_out_handle = -1;
        }

        for (const s32 handle : port.audioout_handles) {
            AudioOut::sceAudioOutClose(handle);
        }
        port.audioout_handles.clear();

        for (auto& data : port.mixed_queue) {
            std::free(data.sample_buffer);
        }

        for (auto& data : port.bed_queue) {
            std::free(data.sample_buffer);
        }

    }

    state->ports.erase(port_id);
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceAudio3dPortCreate(u32 granularity, u32 rate, s64 reserved,
                                      OrbisAudio3dPortId* port_id) {

    LOG_INFO(Lib_Audio3d, "called, granularity = {}, rate = {}, reserved = {}, port_id = {}",
             granularity, rate, reserved, static_cast<void*>(port_id));

    if (!port_id || reserved) {
        LOG_INFO(Lib_Audio3d, "!port_id || reserved");
        return ORBIS_AUDIO3D_ERROR_INVALID_PARAMETER;
    }

    OrbisAudio3dOpenParameters local_params{};
    local_params.size_this = 0x10;
    local_params.granularity = granularity;
    local_params.rate = static_cast<OrbisAudio3dRate>(rate);
    return sceAudio3dPortOpen(static_cast<Libraries::UserService::OrbisUserServiceUserId>(0xFF),
                              &local_params, port_id);
}

s32 PS4_SYSV_ABI sceAudio3dPortDestroy() {
    LOG_ERROR(Lib_Audio3d, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceAudio3dPortFlush(const OrbisAudio3dPortId port_id) {
    LOG_DEBUG(Lib_Audio3d, "called, port_id = {}", port_id);

    if (!state->ports.contains(port_id)) {
        LOG_ERROR(Lib_Audio3d, "!state->ports.contains(port_id)");
        return ORBIS_AUDIO3D_ERROR_INVALID_PORT;
    }

    auto& port = state->ports[port_id];
    std::scoped_lock lock{port.mutex};

    if (!port.audioout_handles.empty()) {
        for (const s32 handle : port.audioout_handles) {
            const s32 ret = AudioOut::sceAudioOutOutput(handle, nullptr);
            if (ret < 0) {
                return ret;
            }
        }
        return ORBIS_OK;
    }

    if (port.mixed_queue.empty()) {
        // Only mix if there's actually something to mix.
        if (!port.bed_queue.empty() ||
            std::any_of(port.objects.begin(), port.objects.end(),
                        [](const auto& kv) { return kv.second.has_pcm; })) {
            const s32 ret = sceAudio3dPortAdvance(port_id);
            if (ret != ORBIS_OK && ret != ORBIS_AUDIO3D_ERROR_NOT_READY) {
                return ret;
            }
        }
    }

    if (port.mixed_queue.empty()) {
        return ORBIS_OK;
    }

    if (port.audio_out_handle < 0) {
        AudioOut::OrbisAudioOutParamExtendedInformation ext_info{};
        ext_info.data_format.Assign(AUDIO3D_OUTPUT_FORMAT);
        port.audio_out_handle =
            AudioOut::sceAudioOutOpen(0xFF, AudioOut::OrbisAudioOutPort::Audio3d, 0,
                                      port.parameters.granularity, AUDIO3D_SAMPLE_RATE, ext_info);
        if (port.audio_out_handle < 0) {
            return port.audio_out_handle;
        }
    }

    // Drain all queued mixed frames, blocking on each until consumed.
    while (!port.mixed_queue.empty()) {
        AudioData frame = port.mixed_queue.front();
        port.mixed_queue.pop_front();
        const s32 ret = AudioOut::sceAudioOutOutput(port.audio_out_handle, frame.sample_buffer);
        std::free(frame.sample_buffer);
        if (ret < 0) {
            return ret;
        }
    }

    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceAudio3dPortFreeState() {
    LOG_ERROR(Lib_Audio3d, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceAudio3dPortGetAttributesSupported(OrbisAudio3dPortId port_id,
                                                      OrbisAudio3dAttributeId* capabilities,
                                                      u32* num_capabilities) {
    LOG_DEBUG(Lib_Audio3d, "called");
    if (!num_capabilities) {
        return ORBIS_AUDIO3D_ERROR_INVALID_PARAMETER;
    }

    if (!state->ports.contains(port_id)) {
        return ORBIS_AUDIO3D_ERROR_INVALID_PORT;
    }

    static constexpr OrbisAudio3dAttributeId Supported[] = {
        OrbisAudio3dAttributeId::ORBIS_AUDIO3D_ATTRIBUTE_PCM,
        OrbisAudio3dAttributeId::ORBIS_AUDIO3D_ATTRIBUTE_PRIORITY,
        OrbisAudio3dAttributeId::ORBIS_AUDIO3D_ATTRIBUTE_POSITION,
        OrbisAudio3dAttributeId::ORBIS_AUDIO3D_ATTRIBUTE_SPREAD,
        OrbisAudio3dAttributeId::ORBIS_AUDIO3D_ATTRIBUTE_GAIN,
        OrbisAudio3dAttributeId::ORBIS_AUDIO3D_ATTRIBUTE_PASSTHROUGH,
        OrbisAudio3dAttributeId::ORBIS_AUDIO3D_ATTRIBUTE_RESET_STATE,
    };
    const u32 supported = static_cast<u32>(std::size(Supported));
    if (capabilities) {
        // Writes up to num_capabilities supported capabilities,
        // then sets num_capabilities to how many were written.
        const u32 count = std::min(*num_capabilities, supported);
        std::copy_n(Supported, count, capabilities);
        *num_capabilities = count;
    } else {
        // If capabilities is null, then just report the number of supported capabilities.
        *num_capabilities = supported;
    }
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceAudio3dPortGetList() {
    LOG_ERROR(Lib_Audio3d, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceAudio3dPortGetParameters() {
    LOG_ERROR(Lib_Audio3d, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceAudio3dPortGetQueueLevel(const OrbisAudio3dPortId port_id, u32* queue_level,
                                             u32* queue_available) {
    LOG_DEBUG(Lib_Audio3d, "called, port_id = {}, queue_level = {}, queue_available = {}", port_id,
              static_cast<void*>(queue_level), static_cast<void*>(queue_available));

    if (!state->ports.contains(port_id)) {
        LOG_ERROR(Lib_Audio3d, "!state->ports.contains(port_id)");
        return ORBIS_AUDIO3D_ERROR_INVALID_PORT;
    }

    if (!queue_level && !queue_available) {
        return ORBIS_AUDIO3D_ERROR_INVALID_PARAMETER;
    }

    const auto& port = state->ports[port_id];
    std::scoped_lock lock{port.mutex};
    const size_t size = port.mixed_queue.size();

    if (queue_level) {
        *queue_level = static_cast<u32>(size);
    }

    if (queue_available) {
        const u32 depth = port.parameters.queue_depth;
        *queue_available = (size < depth) ? static_cast<u32>(depth - size) : 0u;
    }

    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceAudio3dPortGetState() {
    LOG_ERROR(Lib_Audio3d, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceAudio3dPortGetStatus() {
    LOG_ERROR(Lib_Audio3d, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceAudio3dPortOpen(const Libraries::UserService::OrbisUserServiceUserId user_id,
                                    const OrbisAudio3dOpenParameters* parameters,
                                    OrbisAudio3dPortId* port_id) {
    LOG_INFO(Lib_Audio3d, "called, user_id = {}, parameters = {}, port_id = {}", user_id,
             static_cast<const void*>(parameters), static_cast<void*>(port_id));

    if (user_id != 0xFF || !parameters || !port_id) {
        LOG_ERROR(Lib_Audio3d, "user_id != 0xFF || !parameters || !port_id");
        if (port_id)
            *port_id = ORBIS_AUDIO3D_PORT_INVALID;

        return ORBIS_AUDIO3D_ERROR_INVALID_PARAMETER;
    }

    *port_id = ORBIS_AUDIO3D_PORT_INVALID;

    if (!state) {
        LOG_ERROR(Lib_Audio3d, "!initialized");
        return ORBIS_AUDIO3D_ERROR_NOT_READY;
    }

    OrbisAudio3dOpenParameters effective{
        .size_this = 0x28,
        .granularity = parameters->granularity,
        .rate = parameters->rate,
        .max_objects = 512,
        .queue_depth = 2,
        .buffer_mode = OrbisAudio3dBufferMode::ORBIS_AUDIO3D_BUFFER_NO_ADVANCE,
        ._pad = 0,
        .num_beds = 2,
    };

    switch (parameters->size_this & ~0x7ull) {
    case 0x10:
        break;

    case 0x18:
        effective.max_objects = parameters->max_objects;
        effective.queue_depth = parameters->queue_depth;
        effective.buffer_mode = OrbisAudio3dBufferMode::ORBIS_AUDIO3D_BUFFER_ADVANCE_NO_PUSH;
        break;

    case 0x20:
        effective.max_objects = parameters->max_objects;
        effective.queue_depth = parameters->queue_depth;
        effective.buffer_mode = parameters->buffer_mode;
        break;

    case 0x28:
        effective.max_objects = parameters->max_objects;
        effective.queue_depth = parameters->queue_depth;
        effective.buffer_mode = parameters->buffer_mode;
        effective.num_beds = parameters->num_beds;
        break;

    default:
        LOG_ERROR(Lib_Audio3d, "invalid size_this");
        return ORBIS_AUDIO3D_ERROR_INVALID_PARAMETER;
    }

    if (effective.rate != OrbisAudio3dRate::ORBIS_AUDIO3D_RATE_48000) {
        LOG_ERROR(Lib_Audio3d, "unsupported rate");
        return ORBIS_AUDIO3D_ERROR_INVALID_PARAMETER;
    }

    if (effective.granularity < 0x100) {
        LOG_ERROR(Lib_Audio3d, "granularity < 0x100");
        return ORBIS_AUDIO3D_ERROR_INVALID_PARAMETER;
    }

    if ((effective.granularity & 0xFF) != 0) {
        LOG_ERROR(Lib_Audio3d, "granularity not aligned to 0x100");
        return ORBIS_AUDIO3D_ERROR_INVALID_PARAMETER;
    }

    if (effective.max_objects == 0) {
        LOG_ERROR(Lib_Audio3d, "max_objects == 0");
        return ORBIS_AUDIO3D_ERROR_INVALID_PARAMETER;
    }

    if (effective.queue_depth == 0) {
        LOG_ERROR(Lib_Audio3d, "queue_depth == 0");
        return ORBIS_AUDIO3D_ERROR_INVALID_PARAMETER;
    }

    if (effective.granularity == 0x100 && effective.queue_depth > 0x40) {
        LOG_ERROR(Lib_Audio3d, "queue_depth too large for 0x100 granularity");
        return ORBIS_AUDIO3D_ERROR_INVALID_PARAMETER;
    }

    if (effective.granularity == 0x200 && effective.queue_depth > 0x1F) {
        LOG_ERROR(Lib_Audio3d, "queue_depth too large for 0x200 granularity");
        return ORBIS_AUDIO3D_ERROR_INVALID_PARAMETER;
    }

    if (effective.granularity == 0x300 && effective.queue_depth > 0x14) {
        LOG_ERROR(Lib_Audio3d, "queue_depth too large for 0x300 granularity");
        return ORBIS_AUDIO3D_ERROR_INVALID_PARAMETER;
    }

    if (effective.queue_depth > 0xF && effective.granularity > 0x3FF) {
        LOG_ERROR(Lib_Audio3d, "queue_depth invalid for large granularity");
        return ORBIS_AUDIO3D_ERROR_INVALID_PARAMETER;
    }

    if (static_cast<u32>(effective.buffer_mode) > 2) {
        LOG_ERROR(Lib_Audio3d, "invalid buffer_mode");
        return ORBIS_AUDIO3D_ERROR_INVALID_PARAMETER;
    }

    if ((effective.num_beds & 0xfffffffe) != 2) {
        LOG_ERROR(Lib_Audio3d, "invalid num_beds");
        return ORBIS_AUDIO3D_ERROR_INVALID_PARAMETER;
    }

    if (effective.max_objects > 0x200) {
        LOG_WARNING(Lib_Audio3d, "max_objects {} exceeds limit, clamping to 512",
                    effective.max_objects);
        effective.max_objects = 0x200;
    }

    std::scoped_lock lock{state->ports_mutex};

    OrbisAudio3dPortId id = ORBIS_AUDIO3D_PORT_INVALID;
    for (OrbisAudio3dPortId i = 0; i < MaxPorts; i++) {
        if (!state->ports.contains(i)) {
            id = i;
            break;
        }
    }

    if (id == ORBIS_AUDIO3D_PORT_INVALID) {
        LOG_ERROR(Lib_Audio3d, "no free ports");
        return ORBIS_AUDIO3D_ERROR_OUT_OF_RESOURCES;
    }

    auto& port = state->ports.try_emplace(id).first->second;
    port.parameters = effective;
    LOG_INFO(Lib_Audio3d,
             "port {} opened: granularity {}, max_objects {}, queue_depth {}, buffer_mode {}, "
             "num_beds {} (size_this {:#x})",
             id, effective.granularity, effective.max_objects, effective.queue_depth,
             static_cast<u32>(effective.buffer_mode), effective.num_beds, parameters->size_this);

    *port_id = id;

    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceAudio3dPortPush(const OrbisAudio3dPortId port_id,
                                    const OrbisAudio3dBlocking blocking) {
    LOG_DEBUG(Lib_Audio3d, "called, port_id = {}, blocking = {}", port_id,
              magic_enum::enum_name(blocking));

    if (!state->ports.contains(port_id)) {
        LOG_ERROR(Lib_Audio3d, "!state->ports.contains(port_id)");
        return ORBIS_AUDIO3D_ERROR_INVALID_PORT;
    }

    auto& port = state->ports[port_id];

    if (port.parameters.buffer_mode !=
        OrbisAudio3dBufferMode::ORBIS_AUDIO3D_BUFFER_ADVANCE_AND_PUSH) {
        LOG_ERROR(Lib_Audio3d, "port doesn't have push capability");
        return ORBIS_AUDIO3D_ERROR_NOT_SUPPORTED;
    }

    if (port.audio_out_handle < 0) {
        AudioOut::OrbisAudioOutParamExtendedInformation ext_info{};
        ext_info.data_format.Assign(AUDIO3D_OUTPUT_FORMAT);

        port.audio_out_handle =
            AudioOut::sceAudioOutOpen(0xFF, AudioOut::OrbisAudioOutPort::Audio3d, 0,
                                      port.parameters.granularity, AUDIO3D_SAMPLE_RATE, ext_info);

        if (port.audio_out_handle < 0)
            return port.audio_out_handle;
    }

    // Hands what has been advanced to the output. Each frame waits for the one before it to be
    // taken, which is what paces a synchronous push; an asynchronous one sends a single frame.
    while (true) {
        AudioData frame;
        {
            std::scoped_lock lock{port.mutex};
            if (port.mixed_queue.empty()) {
                break;
            }
            frame = port.mixed_queue.front();
            port.mixed_queue.pop_front();
        }

        const s32 ret = AudioOut::sceAudioOutOutput(port.audio_out_handle, frame.sample_buffer);
        std::free(frame.sample_buffer);
        if (ret < 0) {
            return ret;
        }
        if (blocking == OrbisAudio3dBlocking::ORBIS_AUDIO3D_BLOCKING_ASYNC) {
            break;
        }
    }

    return ORBIS_OK;
}
s32 PS4_SYSV_ABI sceAudio3dPortQueryDebug() {
    LOG_ERROR(Lib_Audio3d, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceAudio3dPortSetAttribute(const OrbisAudio3dPortId port_id,
                                            const OrbisAudio3dAttributeId attribute_id,
                                            void* attribute, const u64 attribute_size) {
    LOG_INFO(Lib_Audio3d,
             "called, port_id = {}, attribute_id = {}, attribute = {}, attribute_size = {}",
             port_id, static_cast<u32>(attribute_id), attribute, attribute_size);

    if (!state->ports.contains(port_id)) {
        LOG_ERROR(Lib_Audio3d, "!state->ports.contains(port_id)");
        return ORBIS_AUDIO3D_ERROR_INVALID_PORT;
    }

    if (!attribute) {
        LOG_ERROR(Lib_Audio3d, "!attribute");
        return ORBIS_AUDIO3D_ERROR_INVALID_PARAMETER;
    }

    // TODO

    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceAudio3dReportRegisterHandler() {
    LOG_ERROR(Lib_Audio3d, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceAudio3dReportUnregisterHandler() {
    LOG_ERROR(Lib_Audio3d, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceAudio3dSetGpuRenderer() {
    LOG_ERROR(Lib_Audio3d, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceAudio3dStrError() {
    LOG_ERROR(Lib_Audio3d, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceAudio3dTerminate() {
    LOG_INFO(Lib_Audio3d, "called");
    if (!state) {
        return ORBIS_AUDIO3D_ERROR_NOT_READY;
    }

    std::vector<OrbisAudio3dPortId> port_ids;
    for (const auto& [id, _] : state->ports) {
        port_ids.push_back(id);
    }
    for (const auto id : port_ids) {
        sceAudio3dPortClose(id);
    }

    state.reset();
    return ORBIS_OK;
}

void RegisterLib(Core::Loader::SymbolsResolver* sym) {
    LIB_FUNCTION("pZlOm1aF3aA", "libSceAudio3d", 1, "libSceAudio3d", sceAudio3dAudioOutClose);
    LIB_FUNCTION("ucEsi62soTo", "libSceAudio3d", 1, "libSceAudio3d", sceAudio3dAudioOutOpen);
    LIB_FUNCTION("7NYEzJ9SJbM", "libSceAudio3d", 1, "libSceAudio3d", sceAudio3dAudioOutOutput);
    LIB_FUNCTION("HbxYY27lK6E", "libSceAudio3d", 1, "libSceAudio3d", sceAudio3dAudioOutOutputs);
    LIB_FUNCTION("9tEwE0GV0qo", "libSceAudio3d", 1, "libSceAudio3d", sceAudio3dBedWrite);
    LIB_FUNCTION("xH4Q9UILL3o", "libSceAudio3d", 1, "libSceAudio3d", sceAudio3dBedWrite2);
    LIB_FUNCTION("lvWMW6vEqFU", "libSceAudio3d", 1, "libSceAudio3d", sceAudio3dCreateSpeakerArray);
    LIB_FUNCTION("8hm6YdoQgwg", "libSceAudio3d", 1, "libSceAudio3d", sceAudio3dDeleteSpeakerArray);
    LIB_FUNCTION("Im+jOoa5WAI", "libSceAudio3d", 1, "libSceAudio3d",
                 sceAudio3dGetDefaultOpenParameters);
    LIB_FUNCTION("kEqqyDkmgdI", "libSceAudio3d", 1, "libSceAudio3d",
                 sceAudio3dGetSpeakerArrayMemorySize);
    LIB_FUNCTION("-R1DukFq7Dk", "libSceAudio3d", 1, "libSceAudio3d",
                 sceAudio3dGetSpeakerArrayMixCoefficients);
    LIB_FUNCTION("-Re+pCWvwjQ", "libSceAudio3d", 1, "libSceAudio3d",
                 sceAudio3dGetSpeakerArrayMixCoefficients2);
    LIB_FUNCTION("UmCvjSmuZIw", "libSceAudio3d", 1, "libSceAudio3d", sceAudio3dInitialize);
    LIB_FUNCTION("jO2tec4dJ2M", "libSceAudio3d", 1, "libSceAudio3d", sceAudio3dObjectReserve);
    LIB_FUNCTION("V1FBFpNIAzk", "libSceAudio3d", 1, "libSceAudio3d", sceAudio3dObjectSetAttribute);
    LIB_FUNCTION("4uyHN9q4ZeU", "libSceAudio3d", 1, "libSceAudio3d", sceAudio3dObjectSetAttributes);
    LIB_FUNCTION("1HXxo-+1qCw", "libSceAudio3d", 1, "libSceAudio3d", sceAudio3dObjectUnreserve);
    LIB_FUNCTION("lw0qrdSjZt8", "libSceAudio3d", 1, "libSceAudio3d", sceAudio3dPortAdvance);
    LIB_FUNCTION("OyVqOeVNtSk", "libSceAudio3d", 1, "libSceAudio3d", sceAudio3dPortClose);
    LIB_FUNCTION("UHFOgVNz0kk", "libSceAudio3d", 1, "libSceAudio3d", sceAudio3dPortCreate);
    LIB_FUNCTION("Mw9mRQtWepY", "libSceAudio3d", 1, "libSceAudio3d", sceAudio3dPortDestroy);
    LIB_FUNCTION("ZOGrxWLgQzE", "libSceAudio3d", 1, "libSceAudio3d", sceAudio3dPortFlush);
    LIB_FUNCTION("uJ0VhGcxCTQ", "libSceAudio3d", 1, "libSceAudio3d", sceAudio3dPortFreeState);
    LIB_FUNCTION("9ZA23Ia46Po", "libSceAudio3d", 1, "libSceAudio3d",
                 sceAudio3dPortGetAttributesSupported);
    LIB_FUNCTION("SEggctIeTcI", "libSceAudio3d", 1, "libSceAudio3d", sceAudio3dPortGetList);
    LIB_FUNCTION("flPcUaXVXcw", "libSceAudio3d", 1, "libSceAudio3d", sceAudio3dPortGetParameters);
    LIB_FUNCTION("YaaDbDwKpFM", "libSceAudio3d", 1, "libSceAudio3d", sceAudio3dPortGetQueueLevel);
    LIB_FUNCTION("CKHlRW2E9dA", "libSceAudio3d", 1, "libSceAudio3d", sceAudio3dPortGetState);
    LIB_FUNCTION("iRX6GJs9tvE", "libSceAudio3d", 1, "libSceAudio3d", sceAudio3dPortGetStatus);
    LIB_FUNCTION("XeDDK0xJWQA", "libSceAudio3d", 1, "libSceAudio3d", sceAudio3dPortOpen);
    LIB_FUNCTION("VEVhZ9qd4ZY", "libSceAudio3d", 1, "libSceAudio3d", sceAudio3dPortPush);
    LIB_FUNCTION("-pzYDZozm+M", "libSceAudio3d", 1, "libSceAudio3d", sceAudio3dPortQueryDebug);
    LIB_FUNCTION("Yq9bfUQ0uJg", "libSceAudio3d", 1, "libSceAudio3d", sceAudio3dPortSetAttribute);
    LIB_FUNCTION("QfNXBrKZeI0", "libSceAudio3d", 1, "libSceAudio3d",
                 sceAudio3dReportRegisterHandler);
    LIB_FUNCTION("psv2gbihC1A", "libSceAudio3d", 1, "libSceAudio3d",
                 sceAudio3dReportUnregisterHandler);
    LIB_FUNCTION("yEYXcbAGK14", "libSceAudio3d", 1, "libSceAudio3d", sceAudio3dSetGpuRenderer);
    LIB_FUNCTION("Aacl5qkRU6U", "libSceAudio3d", 1, "libSceAudio3d", sceAudio3dStrError);
    LIB_FUNCTION("WW1TS2iz5yc", "libSceAudio3d", 1, "libSceAudio3d", sceAudio3dTerminate);
};

} // namespace Libraries::Audio3d
