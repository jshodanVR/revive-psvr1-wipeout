// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "common/logging/log.h"
#include "common/path_util.h"
#include "common/wave_dump.h"
#include "core/libraries/error_codes.h"
#include "core/libraries/ngs2/ngs2_codec.h"
#include "core/libraries/ngs2/ngs2_custom.h"
#include "core/libraries/ngs2/ngs2_dsp.h"
#include "core/libraries/ngs2/ngs2_engine.h"
#include "core/libraries/ngs2/ngs2_error.h"
#include "core/libraries/ngs2/ngs2_mastering.h"
#include "core/libraries/ngs2/ngs2_reverb.h"
#include "core/libraries/ngs2/ngs2_sampler.h"
#include "core/libraries/ngs2/ngs2_submixer.h"
#ifdef SHADPS4_ENABLE_FEX_GUEST_CPU
#include "core/guest_cpu/guest_callback.h"
#endif

namespace Libraries::Ngs2::Engine {

namespace {

using Dsp::MaxChannels;
using Dsp::MaxGrain;

constexpr u32 RackIdSampler = 0x1000;
constexpr u32 RackIdSubmixer = 0x2000;
constexpr u32 RackIdReverb = 0x2001;
constexpr u32 RackIdEqualizer = 0x2002;
constexpr u32 RackIdMastering = 0x3000;
constexpr u32 RackIdCustomSampler = 0x4001;
constexpr u32 RackIdCustomSubmixer = 0x4002;
constexpr u32 RackIdCustomMastering = 0x4003;

constexpr u32 StateFlagInUse = 0x1;
constexpr u32 StateFlagPlaying = 0x2;
constexpr u32 StateFlagPaused = 0x4;
constexpr u32 StateFlagStopped = 0x8;
constexpr u32 StateFlagEmpty = 0x20;

// OrbisNgs2VoiceEventParam::eventId
constexpr u32 EventPlay = 0;
constexpr u32 EventStop = 1;
constexpr u32 EventStopImmediate = 2;
constexpr u32 EventKill = 3;
constexpr u32 EventPause = 4;
constexpr u32 EventResume = 5;

// OrbisNgs2VoiceParamHeader::id. The upper half names the rack type the parameter belongs to.
constexpr u32 ParamMatrixLevels = 0x1;
constexpr u32 ParamPortVolume = 0x2;
constexpr u32 ParamPortMatrix = 0x3;
constexpr u32 ParamPortDelay = 0x4;
constexpr u32 ParamPatch = 0x5;
constexpr u32 ParamKickEvent = 0x6;
constexpr u32 ParamCallback = 0x7;

constexpr u32 SamplerSetup = 0x10000000;
constexpr u32 SamplerAddWaveformBlocks = 0x10000001;
constexpr u32 SamplerReplaceWaveformAddress = 0x10000002;
constexpr u32 SamplerSetWaveformFrameOffset = 0x10000003;
constexpr u32 SamplerExitLoop = 0x10000004;
constexpr u32 SamplerSetPitch = 0x10000005;
constexpr u32 SamplerSetEnvelope = 0x10000006;
constexpr u32 SamplerSetDistortion = 0x10000007;
constexpr u32 SamplerSetUserFx = 0x10000008;
constexpr u32 SamplerSetPeakMeter = 0x10000009;
constexpr u32 SamplerSetFilter = 0x1000000a;

constexpr u32 SubmixerSetup = 0x20000000;
constexpr u32 SubmixerSetEnvelope = 0x20000001;
constexpr u32 SubmixerSetCompressor = 0x20000002;
constexpr u32 SubmixerSetDistortion = 0x20000003;
constexpr u32 SubmixerSetUserFx = 0x20000004;
constexpr u32 SubmixerSetPeakMeter = 0x20000005;
constexpr u32 SubmixerSetFilter = 0x20000006;

constexpr u32 ReverbSetup = 0x20010000;
constexpr u32 ReverbSetI3dl2 = 0x20010001;

constexpr u32 MasteringSetup = 0x30000000;
constexpr u32 MasteringSetMatrix = 0x30000001;
constexpr u32 MasteringSetLfe = 0x30000002;
constexpr u32 MasteringSetLimiter = 0x30000003;
constexpr u32 MasteringSetGain = 0x30000004;
constexpr u32 MasteringSetOutput = 0x30000005;
constexpr u32 MasteringSetPeakMeter = 0x30000006;

constexpr u32 CustomSamplerSetup = 0x40010000;
constexpr u32 CustomSubmixerSetup = 0x40020000;
constexpr u32 CustomMasteringSetup = 0x40030000;
// Parameters of a custom rack's modules are 0x4000MMNN: module type MM, NN-th module.
constexpr u32 CustomModuleParamMask = 0xffff0000;
constexpr u32 CustomModuleParamBase = 0x40000000;
constexpr u32 ModuleChorus = 0x1c;
constexpr u32 ModuleDelay = 0x1d;
constexpr u32 ModulePitchShift = 0x20;

// OrbisNgs2UserFxProcessContext::flags as titles test them: the low two bits ask the effect to
// start from scratch, the next two tell it not to process.
constexpr u32 UserFxFlagReset = 0x1;

constexpr u32 RepeatForever = 0xffffffff;
constexpr u32 MaxFilters = 8;
constexpr double MaxPitchStep = 8.0;

struct Rack;
struct System;
struct Voice;

struct Port {
    OrbisNgs2Handle dest_handle{};
    Voice* dest{};
    u32 dest_input{};
    float volume{1.0f};
    s32 matrix{-1};
    /// Gain applied at the end of the previous block, by destination and source channel, so a
    /// change is spread over a block instead of clicking.
    std::array<float, MaxChannels * MaxChannels> gain{};
};

struct Matrix {
    u32 count{};
    std::array<float, MaxChannels * MaxChannels> levels{};
};

struct FilterSlot {
    Dsp::Biquad biquad;
    u32 channel_mask{};
    std::array<Dsp::BiquadState, MaxChannels> state{};
};

struct Envelope {
    std::vector<OrbisNgs2EnvelopePoint> forward;
    std::vector<OrbisNgs2EnvelopePoint> release;
    bool releasing{};
    bool finished{};
    u32 stage{};
    u32 position{};
    float start{};
    float height{1.0f};

    bool Flat() const {
        return forward.empty() && !releasing;
    }

    void Start() {
        releasing = false;
        finished = false;
        stage = 0;
        position = 0;
        start = 0.0f;
        height = forward.empty() ? 1.0f : 0.0f;
    }

    /// Returns false when there is nothing to fade out with.
    bool Release() {
        if (release.empty()) {
            return false;
        }
        releasing = true;
        finished = false;
        stage = 0;
        position = 0;
        start = height;
        return true;
    }

    float Next() {
        const auto& points = releasing ? release : forward;
        while (stage < points.size()) {
            const auto& point = points[stage];
            if (position < point.duration) {
                ++position;
                height = start + (point.height - start) *
                                     (static_cast<float>(position) /
                                      static_cast<float>(point.duration));
                return height;
            }
            height = point.height;
            start = height;
            position = 0;
            ++stage;
        }
        // The last forward point is held until the voice is released.
        if (releasing) {
            finished = true;
        }
        return height;
    }
};

struct WaveformBlock {
    const u8* base{};
    const u8* data{};
    u32 size{};
    u32 repeats{};
    u32 skip{};
    u32 samples{};
    uintptr_t user_data{};
};

struct SamplerState {
    OrbisNgs2WaveformFormat format{};
    WaveformDecoder decoder;
    bool decodable{};

    std::deque<WaveformBlock> queue;
    /// The title has said that no more blocks will follow the queued ones.
    bool complete{};
    bool exit_loop{};

    // Progress through the block at the front of the queue.
    bool block_started{};
    u32 byte_position{};
    u32 skip_left{};
    u32 samples_left{};
    u32 repeats_done{};
    std::vector<float> fifo;
    size_t fifo_read{};

    float pitch{1.0f};
    double fraction{};
    std::array<std::array<float, 4>, MaxChannels> history{};
    bool source_ended{};

    Envelope envelope;

    u64 decoded_samples{};
    u64 decoded_bytes{};
    uintptr_t user_data{};
    const void* waveform_data{};
};

struct ModuleInfo {
    u32 id{};
    float delay_max_ms{1000.0f};
};

struct Voice {
    Rack* rack{};
    u32 index{};
    u32 state_flags{};
    bool setup{};
    bool playing{};
    bool paused{};
    /// The next block is the first one since the voice was started.
    bool fresh{};
    u32 channels{};
    u32 in_degree{};

    std::vector<Port> ports;
    std::vector<Matrix> matrices;
    std::array<FilterSlot, MaxFilters> filters{};
    bool any_filter{};

    OrbisNgs2UserFxProcessHandler fx_handler{};
    std::array<uintptr_t, 3> fx_user_data{};

    /// What the voices patched into this one have produced for the block being rendered.
    std::unique_ptr<float[]> input;
    bool has_input{};
    u32 idle_blocks{};
    float peak{};

    std::unique_ptr<SamplerState> sampler;

    std::unique_ptr<Dsp::Reverb> reverb;
    std::unique_ptr<Dsp::Chorus> chorus;
    std::unique_ptr<Dsp::TapDelay> delay;
    std::unique_ptr<Dsp::PitchShift> pitch_shift;
    u32 tail_blocks{};

    // Mastering
    u32 output_id{};
    bool limiter_enabled{};
    float limiter_threshold{1.0f};
    float gain_full_band{1.0f};
    float gain_lfe{1.0f};
    Dsp::Limiter limiter;
};

struct Rack {
    System* system{};
    u32 rack_id{};
    u32 serial{};
    OrbisNgs2RackOption option{};
    OrbisNgs2ContextBufferInfo buffer{};
    OrbisNgs2BufferFreeHandler free_handler{};
    uintptr_t user_data{};
    std::vector<ModuleInfo> modules;
    std::vector<std::unique_ptr<Voice>> voices;

    bool IsSampler() const {
        return rack_id == RackIdSampler || rack_id == RackIdCustomSampler;
    }
    bool IsMastering() const {
        return rack_id == RackIdMastering || rack_id == RackIdCustomMastering;
    }
};

struct System {
    u32 serial{};
    OrbisNgs2SystemOption option{};
    OrbisNgs2ContextBufferInfo buffer{};
    OrbisNgs2BufferFreeHandler free_handler{};
    uintptr_t user_data{};
    u32 sample_rate{48000};
    u32 grain_samples{256};
    u64 render_count{};
    std::vector<std::unique_ptr<Rack>> racks;

    /// Voices that take input, sorted so that each comes after everything patched into it.
    std::vector<Voice*> order;
    bool order_dirty{true};

    std::vector<float> source_scratch;
    std::array<std::array<float, MaxGrain>, MaxChannels> scratch{};
    std::vector<Common::WaveDump> dumps;
    bool dumps_opened{};
};

enum class HandleType { System, Rack, Voice };

struct Registry {
    std::recursive_mutex mutex;
    std::unordered_map<OrbisNgs2Handle, HandleType> handles;
    std::vector<std::unique_ptr<System>> systems;
    u32 next_serial{1};
};

Registry& GetRegistry() {
    static Registry registry;
    return registry;
}

template <typename T>
T* Lookup(Registry& registry, OrbisNgs2Handle handle, HandleType type) {
    const auto it = registry.handles.find(handle);
    if (it == registry.handles.end() || it->second != type) {
        return nullptr;
    }
    return reinterpret_cast<T*>(handle);
}

// --- tracing ------------------------------------------------------------------------------------

/// SHADPS4_NGS2_TRACE=<max lines> writes what the title asks of the library to
/// <user>/ngs2_trace.log. It exists to work out how a title uses racks, and costs nothing when
/// the variable is not set.
struct Trace {
    std::FILE* file{};
    long remaining{};

    Trace() {
        const char* value = std::getenv("SHADPS4_NGS2_TRACE");
        if (value == nullptr || *value == '\0' || *value == '0') {
            return;
        }
        remaining = std::max(std::atol(value), 100000L);
        const auto path = Common::FS::GetUserPath(Common::FS::PathType::UserDir) / "ngs2_trace.log";
        file = std::fopen(path.string().c_str(), "w");
    }

    bool Enabled() const {
        return file != nullptr && remaining > 0;
    }

    template <typename... Args>
    void Line(const char* format, Args... args) {
        if (!Enabled()) {
            return;
        }
        std::fprintf(file, format, args...);
        std::fputc('\n', file);
        if (--remaining == 0) {
            std::fputs("-- trace limit reached --\n", file);
        }
        if ((remaining & 0xff) == 0) {
            std::fflush(file);
        }
    }
};

Trace& GetTrace() {
    static Trace trace;
    return trace;
}

std::string Hex(const void* data, size_t size, size_t limit = 160) {
    static constexpr char Digits[] = "0123456789abcdef";
    std::string text;
    const auto* bytes = static_cast<const u8*>(data);
    for (size_t i = 0; i < size && i < limit; ++i) {
        if (i != 0 && (i & 3) == 0) {
            text.push_back(' ');
        }
        text.push_back(Digits[bytes[i] >> 4]);
        text.push_back(Digits[bytes[i] & 15]);
    }
    if (size > limit) {
        text += " ...";
    }
    return text;
}

std::string Floats(const float* values, size_t count, size_t limit = 64) {
    std::string text;
    char buffer[32];
    for (size_t i = 0; i < count && i < limit; ++i) {
        std::snprintf(buffer, sizeof(buffer), "%s%.4g", i != 0 ? "," : "", values[i]);
        text += buffer;
    }
    return text;
}

std::string VoiceName(const Voice& voice) {
    char buffer[48];
    std::snprintf(buffer, sizeof(buffer), "R%u[%x].v%u", voice.rack->serial, voice.rack->rack_id,
                  voice.index);
    return buffer;
}

void TraceParam(const Voice& voice, const OrbisNgs2VoiceParamHeader* header) {
    auto& trace = GetTrace();
    if (!trace.Enabled()) {
        return;
    }
    const std::string name = VoiceName(voice);
    const size_t payload = header->size > sizeof(*header) ? header->size - sizeof(*header) : 0;
    trace.Line("CTL %s id=%08x size=%u next=%d data=%s", name.c_str(), header->id, header->size,
               header->next, Hex(header + 1, payload).c_str());

    // Parameters that point at more data.
    switch (header->id) {
    case ParamMatrixLevels: {
        const auto* param = reinterpret_cast<const OrbisNgs2VoiceMatrixLevelsParam*>(header);
        if (header->size >= sizeof(*param) && param->aLevel != nullptr && param->numLevels <= 64) {
            trace.Line("    matrix %u levels[%u]=%s", param->matrixId, param->numLevels,
                       Floats(param->aLevel, param->numLevels).c_str());
        }
        break;
    }
    case SamplerAddWaveformBlocks: {
        const auto* param =
            reinterpret_cast<const OrbisNgs2SamplerVoiceWaveformBlocksParam*>(header);
        if (header->size >= sizeof(*param)) {
            trace.Line("    blocks data=%p flags=%x num=%u", param->data, param->flags,
                       param->numBlocks);
            for (u32 i = 0; i < param->numBlocks && i < 8 && param->aBlock != nullptr; ++i) {
                const auto& block = param->aBlock[i];
                trace.Line("      [%u] off=%u size=%u repeats=%u skip=%u samples=%u rsv=%u "
                           "user=%llx",
                           i, block.dataOffset, block.dataSize, block.numRepeats,
                           block.numSkipSamples, block.numSamples, block.reserved,
                           static_cast<unsigned long long>(block.userData));
            }
        }
        break;
    }
    case SamplerSetEnvelope:
    case SubmixerSetEnvelope: {
        const auto* param = reinterpret_cast<const OrbisNgs2SamplerVoiceEnvelopeParam*>(header);
        if (header->size >= sizeof(*param) && param->aPoint != nullptr) {
            const u32 count = std::min<u32>(param->numForwardPoints + param->numReleasePoints, 16);
            for (u32 i = 0; i < count; ++i) {
                trace.Line("    envelope[%u] curve=%u duration=%u height=%.4g", i,
                           param->aPoint[i].curve, param->aPoint[i].duration,
                           param->aPoint[i].height);
            }
        }
        break;
    }
    case MasteringSetMatrix: {
        const auto* param = reinterpret_cast<const OrbisNgs2MasteringVoiceMatrixParam*>(header);
        if (header->size >= sizeof(*param) && param->aLevel != nullptr && param->numLevels <= 64) {
            trace.Line("    mastering matrix type=%u levels[%u]=%s", param->type, param->numLevels,
                       Floats(param->aLevel, param->numLevels).c_str());
        }
        break;
    }
    default:
        break;
    }
}

void TraceRackOption(const char* what, u32 rack_id, const OrbisNgs2RackOption* option) {
    auto& trace = GetTrace();
    if (!trace.Enabled()) {
        return;
    }
    if (option == nullptr) {
        trace.Line("%s id=%x option=null", what, rack_id);
        return;
    }
    char name[ORBIS_NGS2_RACK_NAME_LENGTH + 1]{};
    std::memcpy(name, option->name, ORBIS_NGS2_RACK_NAME_LENGTH);
    trace.Line("%s id=%x size=%zu name='%s' flags=%x maxGrain=%u maxVoices=%u maxInputDelay=%u "
               "maxMatrices=%u maxPorts=%u",
               what, rack_id, option->size, name, option->flags, option->maxGrainSamples,
               option->maxVoices, option->maxInputDelayBlocks, option->maxMatrices,
               option->maxPorts);
    const size_t base = sizeof(OrbisNgs2RackOption);
    if (option->size > base && option->size <= 4096) {
        const bool custom = (rack_id & 0xf000) == 0x4000;
        if (!custom) {
            trace.Line("    extra=%s",
                       Hex(reinterpret_cast<const u8*>(option) + base, option->size - base, 96)
                           .c_str());
        } else if (option->size >= sizeof(OrbisNgs2CustomRackOption)) {
            const auto* custom_option = reinterpret_cast<const OrbisNgs2CustomRackOption*>(option);
            trace.Line("    custom stateSize=%u numBuffers=%u numModules=%u",
                       custom_option->stateSize, custom_option->numBuffers,
                       custom_option->numModules);
            for (u32 i = 0; i < custom_option->numModules && i < ORBIS_NGS2_CUSTOM_MAX_MODULES;
                 ++i) {
                const auto& module = custom_option->aModule[i];
                std::string module_option = "null";
                if (module.option != nullptr && module.option->size <= 256) {
                    module_option = Hex(module.option, module.option->size, 96);
                }
                trace.Line("    module[%u] id=%x src=%u extra=%u dst=%u stateOff=%u stateSize=%u "
                           "option=%s",
                           i, module.moduleId, module.sourceBufferId, module.extraBufferId,
                           module.destBufferId, module.stateOffset, module.stateSize,
                           module_option.c_str());
            }
        }
    }
}

// --- voices -------------------------------------------------------------------------------------

float* InputChannel(Voice& voice, u32 channel) {
    return voice.input.get() + size_t{channel} * MaxGrain;
}

void EnsureInput(Voice& voice) {
    if (!voice.input) {
        voice.input = std::make_unique<float[]>(size_t{MaxChannels} * MaxGrain);
        std::fill_n(voice.input.get(), size_t{MaxChannels} * MaxGrain, 0.0f);
    }
}

void ResetFilters(Voice& voice) {
    for (auto& slot : voice.filters) {
        slot = {};
    }
    voice.any_filter = false;
}

/// The voice has nothing more to play: titles watch for the "in use" flag to drop.
void FinishVoice(Voice& voice) {
    voice.playing = false;
    voice.paused = false;
    voice.state_flags = StateFlagStopped;
    if (voice.sampler) {
        voice.sampler->queue.clear();
        voice.sampler->block_started = false;
        voice.sampler->fifo.clear();
        voice.sampler->fifo_read = 0;
    }
}

void StartVoice(Voice& voice) {
    voice.playing = true;
    voice.paused = false;
    voice.fresh = true;
    voice.idle_blocks = 0;
    voice.state_flags = StateFlagInUse | StateFlagPlaying;
    if (voice.sampler) {
        auto& sampler = *voice.sampler;
        sampler.envelope.Start();
        sampler.fraction = 0.0;
        sampler.history = {};
        sampler.source_ended = false;
        if (sampler.queue.empty() && !sampler.block_started) {
            voice.state_flags |= StateFlagEmpty;
        }
    }
    for (auto& slot : voice.filters) {
        slot.state = {};
    }
}

void SetFilter(Voice& voice, u32 index, u32 type, u32 channel_mask, const float* values) {
    if (index >= MaxFilters) {
        return;
    }
    auto& slot = voice.filters[index];
    const float sample_rate = static_cast<float>(voice.rack->system->sample_rate);
    const bool was_identity = slot.biquad.identity;
    slot.biquad = Dsp::MakeFilter(type, values[0], values[1], values[2], sample_rate);
    slot.channel_mask = channel_mask;
    if (was_identity && !slot.biquad.identity) {
        slot.state = {};
    }
    voice.any_filter = std::any_of(voice.filters.begin(), voice.filters.end(),
                                   [](const FilterSlot& entry) { return !entry.biquad.identity; });
    if (type > Dsp::FilterHighShelf) {
        static bool warned = false;
        if (!warned) {
            warned = true;
            LOG_WARNING(Lib_Ngs2, "filter type {} is not implemented", type);
        }
    }
}

void RunFilters(Voice& voice, float* const channels[], u32 num_channels, u32 count) {
    if (!voice.any_filter) {
        return;
    }
    for (auto& slot : voice.filters) {
        if (slot.biquad.identity) {
            continue;
        }
        for (u32 c = 0; c < num_channels; ++c) {
            // No channel named means all of them.
            if (slot.channel_mask == 0 || (slot.channel_mask & (1u << c)) != 0) {
                Dsp::RunBiquad(slot.biquad, slot.state[c], channels[c], count);
            }
        }
    }
}

void RunUserFx(Voice& voice, float* const channels[], u32 num_channels, u32 count) {
    if (voice.fx_handler == nullptr) {
        return;
    }
    std::array<float*, MaxChannels> data{};
    for (u32 c = 0; c < MaxChannels; ++c) {
        data[c] = channels[std::min(c, num_channels - 1)];
    }
    OrbisNgs2UserFxProcessContext context{};
    context.aChannelData = data.data();
    context.userData0 = voice.fx_user_data[0];
    context.userData1 = voice.fx_user_data[1];
    context.userData2 = voice.fx_user_data[2];
    context.flags = voice.fresh ? UserFxFlagReset : 0;
    context.numChannels = num_channels;
    context.numGrainSamples = count;
    context.sampleRate = voice.rack->system->sample_rate;
#ifdef SHADPS4_ENABLE_FEX_GUEST_CPU
    const void* address = reinterpret_cast<const void*>(voice.fx_handler);
    if (Core::GuestCpu::IsGuestFunctionAddress(address)) {
        Core::GuestCpu::RunGuestFunctionOrAbort(address, "Ngs2 user effect", &context);
        return;
    }
#endif
    voice.fx_handler(&context);
}

/// Returns the largest sample of a block. A block holding values no sound has (a filter gone
/// unstable, a title's effect writing garbage) is silenced and reported as such, so that it
/// does not reach the mix or the filters after it.
float PeakOf(float* const channels[], u32 num_channels, u32 count, bool& sane) {
    static constexpr float Limit = 64.0f;
    float peak = 0.0f;
    sane = true;
    for (u32 c = 0; c < num_channels && sane; ++c) {
        for (u32 i = 0; i < count; ++i) {
            const float magnitude = std::abs(channels[c][i]);
            if (!(magnitude <= Limit)) {
                sane = false;
                break;
            }
            peak = std::max(peak, magnitude);
        }
    }
    if (!sane) {
        for (u32 c = 0; c < num_channels; ++c) {
            std::fill_n(channels[c], count, 0.0f);
        }
        return 0.0f;
    }
    return peak;
}

void ReportInsane(const Voice& voice) {
    static u32 reported = 0;
    if (reported < 16) {
        ++reported;
        LOG_WARNING(Lib_Ngs2, "voice {} of rack {:#x} produced an out-of-range block, muted it",
                    voice.index, voice.rack->rack_id);
    }
}

/// Adds a voice's output to the inputs of the voices its ports are patched into.
void MixToPorts(Voice& voice, const float* const channels[], u32 num_channels, u32 count) {
    const float inverse = 1.0f / static_cast<float>(count);
    for (auto& port : voice.ports) {
        Voice* dest = port.dest;
        if (dest == nullptr || !dest->playing || !dest->input) {
            // Start from the full level again once something is listening.
            port.gain.fill(-1.0f);
            continue;
        }
        const Matrix* matrix = nullptr;
        if (port.matrix >= 0 && static_cast<size_t>(port.matrix) < voice.matrices.size() &&
            voice.matrices[port.matrix].count != 0) {
            matrix = &voice.matrices[port.matrix];
        }
        const u32 dest_channels = std::max<u32>(dest->channels, 1);
        bool audible = false;
        for (u32 d = 0; d < dest_channels; ++d) {
            float* out = InputChannel(*dest, d);
            for (u32 s = 0; s < num_channels; ++s) {
                float level;
                if (matrix != nullptr) {
                    // One row of source levels per destination channel.
                    const u32 at = d * num_channels + s;
                    level = at < matrix->count ? matrix->levels[at] : 0.0f;
                } else {
                    level = d == s ? 1.0f : 0.0f;
                }
                const float target = level * port.volume;
                float& previous = port.gain[d * MaxChannels + s];
                const float from = voice.fresh || previous < 0.0f ? target : previous;
                previous = target;
                if (target == 0.0f && from == 0.0f) {
                    continue;
                }
                audible = true;
                const float* in = channels[s];
                if (from == target) {
                    for (u32 i = 0; i < count; ++i) {
                        out[i] += in[i] * target;
                    }
                } else {
                    const float step = (target - from) * inverse;
                    float gain = from;
                    for (u32 i = 0; i < count; ++i) {
                        gain += step;
                        out[i] += in[i] * gain;
                    }
                }
            }
        }
        if (audible) {
            dest->has_input = true;
        }
    }
}

// --- sampler ------------------------------------------------------------------------------------

/// Moves on from the block at the front of the queue: plays it again or drops it.
void EndBlock(SamplerState& sampler) {
    auto& block = sampler.queue.front();
    const bool again = !sampler.exit_loop &&
                       (block.repeats == RepeatForever || sampler.repeats_done < block.repeats);
    if (again) {
        ++sampler.repeats_done;
    } else {
        sampler.queue.pop_front();
        sampler.repeats_done = 0;
        sampler.exit_loop = false;
    }
    sampler.block_started = false;
}

/// Fills `out` (one array per channel) with the next `count` source samples and returns how
/// many there were before the queued data ran out.
u32 PullSource(SamplerState& sampler, float* const out[], u32 count) {
    const u32 num_channels = sampler.decoder.Channels();
    u32 produced = 0;
    u32 guard = 0;
    while (produced < count) {
        if (sampler.queue.empty()) {
            break;
        }
        auto& block = sampler.queue.front();
        if (!sampler.block_started) {
            sampler.block_started = true;
            sampler.byte_position = 0;
            sampler.skip_left = block.skip;
            // A block that does not say how long it is plays to the end of its data.
            sampler.samples_left = block.samples != 0 ? block.samples : 0xffffffff;
            sampler.fifo.clear();
            sampler.fifo_read = 0;
            sampler.decoder.Restart();
            sampler.user_data = block.user_data;
            // A block that yields nothing must not spin here forever.
            if (++guard > 64) {
                sampler.queue.clear();
                sampler.block_started = false;
                break;
            }
        }

        size_t available = (sampler.fifo.size() - sampler.fifo_read) / num_channels;
        if (available == 0) {
            sampler.fifo.clear();
            sampler.fifo_read = 0;
            const size_t used =
                sampler.byte_position < block.size
                    ? sampler.decoder.DecodeFrame(block.data + sampler.byte_position,
                                                  block.size - sampler.byte_position, sampler.fifo)
                    : 0;
            if (used == 0) {
                // Out of data before the announced number of samples.
                EndBlock(sampler);
                continue;
            }
            sampler.byte_position += static_cast<u32>(used);
            sampler.decoded_bytes += used;
            sampler.waveform_data = block.data + sampler.byte_position;
            available = sampler.fifo.size() / num_channels;
        }

        if (sampler.skip_left != 0) {
            const u32 skipped = static_cast<u32>(std::min<size_t>(sampler.skip_left, available));
            sampler.fifo_read += size_t{skipped} * num_channels;
            sampler.skip_left -= skipped;
            continue;
        }

        const u32 take = static_cast<u32>(
            std::min<size_t>({available, count - produced, sampler.samples_left}));
        const float* source = sampler.fifo.data() + sampler.fifo_read;
        for (u32 c = 0; c < num_channels; ++c) {
            float* target = out[c] + produced;
            for (u32 i = 0; i < take; ++i) {
                target[i] = source[i * num_channels + c];
            }
        }
        sampler.fifo_read += size_t{take} * num_channels;
        sampler.samples_left -= take;
        sampler.decoded_samples += take;
        produced += take;
        guard = 0;
        if (sampler.samples_left == 0) {
            EndBlock(sampler);
        }
    }
    return produced;
}

void RenderSampler(System& system, Voice& voice, u32 count) {
    auto& sampler = *voice.sampler;
    const u32 num_channels = sampler.decoder.Channels();
    if (!sampler.decodable || num_channels == 0) {
        FinishVoice(voice);
        return;
    }

    // How far the source advances per output sample.
    const double step = std::clamp(static_cast<double>(sampler.pitch) * sampler.format.sampleRate /
                                       system.sample_rate,
                                   1.0 / 64.0, MaxPitchStep);
    const u32 needed = static_cast<u32>(sampler.fraction + step * count);

    // Per channel: the four samples carried over, then the new ones.
    const size_t stride = static_cast<size_t>(MaxPitchStep * MaxGrain) + 8;
    system.source_scratch.resize(stride * MaxChannels);
    std::array<float*, MaxChannels> source{};
    std::array<float*, MaxChannels> fresh{};
    for (u32 c = 0; c < num_channels; ++c) {
        source[c] = system.source_scratch.data() + stride * c;
        fresh[c] = source[c] + 4;
        std::copy(sampler.history[c].begin(), sampler.history[c].end(), source[c]);
    }

    const u32 produced = needed != 0 ? PullSource(sampler, fresh.data(), needed) : 0;
    if (produced < needed) {
        for (u32 c = 0; c < num_channels; ++c) {
            std::fill(fresh[c] + produced, fresh[c] + needed, 0.0f);
        }
        if (sampler.queue.empty() && sampler.complete) {
            sampler.source_ended = true;
        }
    }
    const bool empty = sampler.queue.empty() && !sampler.block_started;
    voice.state_flags = (voice.state_flags & ~StateFlagEmpty) | (empty ? StateFlagEmpty : 0);

    // Cubic interpolation through the two samples around the read position and their
    // neighbours.
    std::array<float*, MaxChannels> out{};
    for (u32 c = 0; c < num_channels; ++c) {
        out[c] = system.scratch[c].data();
        const float* in = source[c];
        double position = sampler.fraction;
        for (u32 i = 0; i < count; ++i) {
            const u32 whole = static_cast<u32>(position);
            const float t = static_cast<float>(position - whole);
            const float p0 = in[whole];
            const float p1 = in[whole + 1];
            const float p2 = in[whole + 2];
            const float p3 = in[whole + 3];
            const float a = 0.5f * (p3 - p0) + 1.5f * (p1 - p2);
            const float b = p0 - 2.5f * p1 + 2.0f * p2 - 0.5f * p3;
            const float d = 0.5f * (p2 - p0);
            out[c][i] = ((a * t + b) * t + d) * t + p1;
            position += step;
        }
        std::copy(in + needed, in + needed + 4, sampler.history[c].begin());
    }
    sampler.fraction = sampler.fraction + step * count - needed;

    if (!sampler.envelope.Flat()) {
        for (u32 i = 0; i < count; ++i) {
            const float height = sampler.envelope.Next();
            for (u32 c = 0; c < num_channels; ++c) {
                out[c][i] *= height;
            }
        }
    }

    RunFilters(voice, out.data(), num_channels, count);
    RunUserFx(voice, out.data(), num_channels, count);
    bool sane = true;
    voice.peak = PeakOf(out.data(), num_channels, count, sane);
    if (!sane) {
        ReportInsane(voice);
        for (auto& slot : voice.filters) {
            slot.state = {};
        }
    }
    MixToPorts(voice, out.data(), num_channels, count);
    voice.fresh = false;

    if (sampler.source_ended || sampler.envelope.finished) {
        FinishVoice(voice);
    }
}

// --- busses -------------------------------------------------------------------------------------

void WriteOutput(const OrbisNgs2RenderBufferInfo& info, const float* const channels[], u32 count) {
    if (info.buffer == nullptr || info.numChannels == 0) {
        return;
    }
    const u32 out_channels = info.numChannels;
    const u32 used_channels = std::min<u32>(out_channels, MaxChannels);
    if (info.waveformType == WaveformPcmF32L) {
        const u32 frames =
            static_cast<u32>(std::min<size_t>(count, info.bufferSize / (4 * out_channels)));
        auto* out = static_cast<float*>(info.buffer);
        for (u32 c = 0; c < used_channels; ++c) {
            const float* in = channels[c];
            for (u32 i = 0; i < frames; ++i) {
                out[i * out_channels + c] = in[i];
            }
        }
    } else if (info.waveformType == WaveformPcmI16L) {
        const u32 frames =
            static_cast<u32>(std::min<size_t>(count, info.bufferSize / (2 * out_channels)));
        auto* out = static_cast<s16*>(info.buffer);
        for (u32 c = 0; c < used_channels; ++c) {
            const float* in = channels[c];
            for (u32 i = 0; i < frames; ++i) {
                out[i * out_channels + c] =
                    static_cast<s16>(std::clamp(in[i], -1.0f, 1.0f) * 32767.0f);
            }
        }
    }
}

void RenderBus(System& system, Voice& voice, const OrbisNgs2RenderBufferInfo* buffers,
               u32 num_buffers, u32 count) {
    // A bus nothing has fed for longer than its effects ring is left alone. One that runs the
    // title's own effect code is always processed: that code may well be the source.
    if (voice.has_input) {
        voice.idle_blocks = 0;
    } else if (voice.fx_handler == nullptr && ++voice.idle_blocks > voice.tail_blocks) {
        voice.idle_blocks = voice.tail_blocks + 1;
        voice.peak = 0.0f;
        for (auto& port : voice.ports) {
            port.gain.fill(-1.0f);
        }
        return;
    }

    const u32 num_channels = std::clamp<u32>(voice.channels, 1, MaxChannels);
    std::array<float*, MaxChannels> channels{};
    for (u32 c = 0; c < MaxChannels; ++c) {
        channels[c] = InputChannel(voice, c);
    }

    const u32 rack_id = voice.rack->rack_id;
    if (rack_id == RackIdReverb) {
        if (voice.reverb) {
            voice.reverb->Process(channels.data(), count);
        }
    } else if (rack_id == RackIdCustomSubmixer) {
        if (voice.pitch_shift) {
            voice.pitch_shift->Process(channels.data(), num_channels, count);
        }
        if (voice.chorus) {
            voice.chorus->Process(channels.data(), num_channels, count);
        }
        if (voice.delay) {
            voice.delay->Process(channels.data(), num_channels, count);
        }
    }

    RunFilters(voice, channels.data(), num_channels, count);
    RunUserFx(voice, channels.data(), num_channels, count);

    if (voice.rack->IsMastering()) {
        if (voice.gain_full_band != 1.0f || voice.gain_lfe != 1.0f) {
            for (u32 c = 0; c < num_channels; ++c) {
                const float gain = c == 3 ? voice.gain_lfe : voice.gain_full_band;
                for (u32 i = 0; i < count; ++i) {
                    channels[c][i] *= gain;
                }
            }
        }
        if (voice.limiter_enabled) {
            voice.limiter.Process(channels.data(), num_channels, count, voice.limiter_threshold);
        }
        bool sane = true;
        voice.peak = PeakOf(channels.data(), num_channels, count, sane);
        if (!sane) {
            ReportInsane(voice);
        }
        if (voice.output_id < num_buffers) {
            WriteOutput(buffers[voice.output_id], channels.data(), count);
        }
        if (voice.output_id < system.dumps.size()) {
            system.dumps[voice.output_id].WritePlanar(channels.data(), count);
        }
    } else {
        bool sane = true;
        voice.peak = PeakOf(channels.data(), num_channels, count, sane);
        if (!sane) {
            ReportInsane(voice);
            for (auto& slot : voice.filters) {
                slot.state = {};
            }
        }
        MixToPorts(voice, channels.data(), num_channels, count);
    }
    voice.fresh = false;

    std::fill_n(voice.input.get(), size_t{MaxChannels} * MaxGrain, 0.0f);
    voice.has_input = false;
}

/// Sorts the voices that take input so that each is rendered after its sources.
void RebuildOrder(System& system) {
    std::vector<Voice*> nodes;
    for (const auto& rack : system.racks) {
        if (rack->IsSampler()) {
            continue;
        }
        for (const auto& voice : rack->voices) {
            voice->in_degree = 0;
            nodes.push_back(voice.get());
        }
    }
    for (Voice* voice : nodes) {
        for (const auto& port : voice->ports) {
            if (port.dest != nullptr && port.dest != voice) {
                ++port.dest->in_degree;
            }
        }
    }
    system.order.clear();
    std::deque<Voice*> ready;
    for (Voice* voice : nodes) {
        if (voice->in_degree == 0) {
            ready.push_back(voice);
        }
    }
    while (!ready.empty()) {
        Voice* voice = ready.front();
        ready.pop_front();
        system.order.push_back(voice);
        for (const auto& port : voice->ports) {
            if (port.dest != nullptr && port.dest != voice && port.dest->in_degree != 0 &&
                --port.dest->in_degree == 0) {
                ready.push_back(port.dest);
            }
        }
    }
    // Voices patched in a circle have no right order; they still get rendered.
    if (system.order.size() != nodes.size()) {
        for (Voice* voice : nodes) {
            if (std::find(system.order.begin(), system.order.end(), voice) ==
                system.order.end()) {
                system.order.push_back(voice);
            }
        }
    }
    system.order_dirty = false;
}

void OpenDumps(System& system, u32 num_buffers, const OrbisNgs2RenderBufferInfo* buffers) {
    system.dumps_opened = true;
    const char* value = std::getenv("SHADPS4_NGS2_DUMP");
    if (value == nullptr || std::atoi(value) <= 0) {
        return;
    }
    const u64 limit = static_cast<u64>(std::atoi(value)) * system.sample_rate;
    system.dumps.resize(num_buffers);
    for (u32 i = 0; i < num_buffers; ++i) {
        const auto path = Common::FS::GetUserPath(Common::FS::PathType::UserDir) /
                          ("ngs2_out" + std::to_string(i) + ".wav");
        system.dumps[i].Open(path.string(), std::clamp<u32>(buffers[i].numChannels, 1, 8),
                             system.sample_rate, limit);
    }
}

Voice* ResolvePatchTarget(Registry& registry, OrbisNgs2Handle handle) {
    if (handle == 0) {
        return nullptr;
    }
    if (auto* voice = Lookup<Voice>(registry, handle, HandleType::Voice)) {
        return voice;
    }
    // Titles also patch to a rack, meaning its first voice.
    if (auto* rack = Lookup<Rack>(registry, handle, HandleType::Rack)) {
        return rack->voices.empty() ? nullptr : rack->voices.front().get();
    }
    return nullptr;
}

u32 DefaultMaxVoices(u32 rack_id) {
    switch (rack_id) {
    case RackIdSampler:
    case RackIdCustomSampler:
        return 256;
    default:
        return 1;
    }
}

u32 TailBlocks(const System& system, float seconds) {
    return static_cast<u32>(seconds * static_cast<float>(system.sample_rate) /
                            static_cast<float>(system.grain_samples)) +
           1;
}

// --- voice parameters ---------------------------------------------------------------------------

s32 SetupSampler(Voice& voice, const OrbisNgs2WaveformFormat& format) {
    if (!voice.sampler) {
        voice.sampler = std::make_unique<SamplerState>();
    }
    auto& sampler = *voice.sampler;
    sampler.format = format;
    sampler.decodable = sampler.decoder.Setup(format);
    sampler.queue.clear();
    sampler.complete = false;
    sampler.exit_loop = false;
    sampler.block_started = false;
    sampler.repeats_done = 0;
    sampler.fifo.clear();
    sampler.fifo_read = 0;
    sampler.pitch = 1.0f;
    sampler.fraction = 0.0;
    sampler.history = {};
    sampler.source_ended = false;
    sampler.envelope = {};
    sampler.decoded_samples = 0;
    sampler.decoded_bytes = 0;
    sampler.user_data = 0;
    sampler.waveform_data = nullptr;

    voice.setup = true;
    voice.playing = false;
    voice.paused = false;
    voice.channels = sampler.decoder.Channels();
    voice.fx_handler = nullptr;
    voice.fx_user_data = {};
    voice.peak = 0.0f;
    ResetFilters(voice);
    voice.state_flags = StateFlagInUse | StateFlagEmpty;
    if (!sampler.decodable) {
        return ORBIS_NGS2_ERROR_INVALID_WAVEFORM_FORMAT;
    }
    return ORBIS_OK;
}

void SetupBus(Voice& voice, u32 num_channels) {
    System& system = *voice.rack->system;
    EnsureInput(voice);
    voice.setup = true;
    voice.channels = std::clamp<u32>(num_channels, 1, MaxChannels);
    voice.state_flags = StateFlagInUse | (voice.playing ? StateFlagPlaying : 0);
    voice.tail_blocks = TailBlocks(system, 0.5f);
    const float sample_rate = static_cast<float>(system.sample_rate);

    if (voice.rack->rack_id == RackIdReverb) {
        voice.reverb = std::make_unique<Dsp::Reverb>();
        voice.reverb->Setup(sample_rate);
        voice.tail_blocks = TailBlocks(system, 4.0f);
    } else if (voice.rack->rack_id == RackIdCustomSubmixer) {
        for (const auto& module : voice.rack->modules) {
            if (module.id == ModuleChorus && !voice.chorus) {
                voice.chorus = std::make_unique<Dsp::Chorus>();
                voice.chorus->Setup(sample_rate);
                voice.tail_blocks = std::max(voice.tail_blocks, TailBlocks(system, 2.0f));
            } else if (module.id == ModuleDelay && !voice.delay) {
                voice.delay = std::make_unique<Dsp::TapDelay>();
                voice.delay->Setup(sample_rate, module.delay_max_ms);
                voice.tail_blocks = std::max(voice.tail_blocks, TailBlocks(system, 10.0f));
            } else if (module.id == ModulePitchShift && !voice.pitch_shift) {
                voice.pitch_shift = std::make_unique<Dsp::PitchShift>();
                voice.pitch_shift->Setup(sample_rate);
            }
        }
    } else if (voice.rack->IsMastering()) {
        voice.limiter.Setup(sample_rate);
    }
    system.order_dirty = true;
}

void SetEnvelope(Envelope& envelope, u32 num_forward, u32 num_release,
                 const OrbisNgs2EnvelopePoint* points) {
    envelope.forward.clear();
    envelope.release.clear();
    if (points == nullptr || num_forward > 64 || num_release > 64) {
        return;
    }
    envelope.forward.assign(points, points + num_forward);
    envelope.release.assign(points + num_forward, points + num_forward + num_release);
}

s32 KickEvent(Voice& voice, u32 event) {
    switch (event) {
    case EventPlay:
        if (!voice.setup) {
            return ORBIS_NGS2_ERROR_UNINIT_VOICE;
        }
        if (!voice.playing) {
            StartVoice(voice);
        } else if (voice.paused) {
            voice.paused = false;
            voice.state_flags &= ~StateFlagPaused;
        }
        return ORBIS_OK;
    case EventStop:
        // A sampler voice fades out along its release curve first, if it has one.
        if (voice.playing && voice.sampler && !voice.paused && voice.sampler->envelope.Release()) {
            return ORBIS_OK;
        }
        [[fallthrough]];
    case EventStopImmediate:
        if (voice.playing || voice.sampler) {
            FinishVoice(voice);
        }
        if (!voice.sampler) {
            voice.state_flags = voice.setup ? StateFlagInUse : 0;
        }
        return ORBIS_OK;
    case EventKill:
        FinishVoice(voice);
        voice.state_flags = 0;
        return ORBIS_OK;
    case EventPause:
        if (voice.playing) {
            voice.paused = true;
            voice.state_flags |= StateFlagPaused;
        }
        return ORBIS_OK;
    case EventResume:
        if (voice.playing) {
            voice.paused = false;
            voice.state_flags &= ~StateFlagPaused;
        }
        return ORBIS_OK;
    default:
        return ORBIS_NGS2_ERROR_INVALID_EVENT_TYPE;
    }
}

/// Reads a parameter as `T` if the title passed at least that much.
template <typename T>
const T* As(const OrbisNgs2VoiceParamHeader* header) {
    return header->size >= sizeof(T) ? reinterpret_cast<const T*>(header) : nullptr;
}

s32 ApplyCommonParam(Registry& registry, Voice& voice, const OrbisNgs2VoiceParamHeader* header,
                     bool& handled) {
    handled = true;
    System& system = *voice.rack->system;
    switch (header->id) {
    case ParamMatrixLevels: {
        const auto* param = As<OrbisNgs2VoiceMatrixLevelsParam>(header);
        if (param == nullptr) {
            return ORBIS_NGS2_ERROR_INVALID_VOICE_CONTROL_SIZE;
        }
        if (param->matrixId >= voice.matrices.size()) {
            return ORBIS_NGS2_ERROR_INVALID_MATRIX_INDEX;
        }
        if (param->numLevels > MaxChannels * MaxChannels) {
            return ORBIS_NGS2_ERROR_INVALID_NUM_MATRIX_LEVELS;
        }
        if (param->aLevel == nullptr && param->numLevels != 0) {
            return ORBIS_NGS2_ERROR_INVALID_MATRIX_LEVEL_ADDRESS;
        }
        auto& matrix = voice.matrices[param->matrixId];
        matrix.count = param->numLevels;
        for (u32 i = 0; i < param->numLevels; ++i) {
            const float level = param->aLevel[i];
            matrix.levels[i] = std::isfinite(level) ? level : 0.0f;
        }
        return ORBIS_OK;
    }
    case ParamPortVolume: {
        const auto* param = As<OrbisNgs2VoicePortVolumeParam>(header);
        if (param == nullptr) {
            return ORBIS_NGS2_ERROR_INVALID_VOICE_CONTROL_SIZE;
        }
        if (param->port >= voice.ports.size()) {
            return ORBIS_NGS2_ERROR_INVALID_PORT_INDEX;
        }
        voice.ports[param->port].volume = std::isfinite(param->level) ? param->level : 0.0f;
        return ORBIS_OK;
    }
    case ParamPortMatrix: {
        const auto* param = As<OrbisNgs2VoicePortMatrixParam>(header);
        if (param == nullptr) {
            return ORBIS_NGS2_ERROR_INVALID_VOICE_CONTROL_SIZE;
        }
        if (param->port >= voice.ports.size()) {
            return ORBIS_NGS2_ERROR_INVALID_PORT_INDEX;
        }
        voice.ports[param->port].matrix = param->matrixId;
        return ORBIS_OK;
    }
    case ParamPortDelay:
        return ORBIS_OK;
    case ParamPatch: {
        const auto* param = As<OrbisNgs2VoicePatchParam>(header);
        if (param == nullptr) {
            return ORBIS_NGS2_ERROR_INVALID_VOICE_CONTROL_SIZE;
        }
        if (param->port >= voice.ports.size()) {
            return ORBIS_NGS2_ERROR_INVALID_PORT_INDEX;
        }
        auto& port = voice.ports[param->port];
        Voice* dest = ResolvePatchTarget(registry, param->destHandle);
        if (dest != nullptr && (dest->rack->IsSampler() || dest->rack->system != &system)) {
            return ORBIS_NGS2_ERROR_INVALID_PATCH;
        }
        if (port.dest != dest) {
            port.dest = dest;
            port.gain.fill(-1.0f);
            system.order_dirty = true;
        }
        port.dest_handle = param->destHandle;
        port.dest_input = param->destInputId;
        return param->destHandle != 0 && dest == nullptr ? ORBIS_NGS2_ERROR_INVALID_PATCH
                                                         : ORBIS_OK;
    }
    case ParamKickEvent: {
        const auto* param = As<OrbisNgs2VoiceEventParam>(header);
        if (param == nullptr) {
            return ORBIS_NGS2_ERROR_INVALID_VOICE_CONTROL_SIZE;
        }
        return KickEvent(voice, param->eventId);
    }
    case ParamCallback:
        return ORBIS_OK;
    default:
        handled = false;
        return ORBIS_OK;
    }
}

s32 ApplySamplerParam(Voice& voice, const OrbisNgs2VoiceParamHeader* header) {
    if (header->id == SamplerSetup || header->id == CustomSamplerSetup) {
        const auto* param = As<OrbisNgs2SamplerVoiceSetupParam>(header);
        if (param == nullptr) {
            return ORBIS_NGS2_ERROR_INVALID_VOICE_CONTROL_SIZE;
        }
        return SetupSampler(voice, param->format);
    }
    if (!voice.sampler) {
        return ORBIS_NGS2_ERROR_UNINIT_VOICE;
    }
    auto& sampler = *voice.sampler;
    switch (header->id) {
    case SamplerAddWaveformBlocks: {
        const auto* param = As<OrbisNgs2SamplerVoiceWaveformBlocksParam>(header);
        if (param == nullptr) {
            return ORBIS_NGS2_ERROR_INVALID_VOICE_CONTROL_SIZE;
        }
        if (param->numBlocks != 0 && (param->aBlock == nullptr || param->data == nullptr)) {
            return ORBIS_NGS2_ERROR_INVALID_WAVEFORM_BLOCK_ADDRESS;
        }
        if (param->numBlocks > 4096) {
            return ORBIS_NGS2_ERROR_INVALID_NUM_WAVEFORM_BLOCKS;
        }
        for (u32 i = 0; i < param->numBlocks; ++i) {
            const auto& block = param->aBlock[i];
            WaveformBlock entry;
            entry.base = static_cast<const u8*>(param->data);
            entry.data = entry.base + block.dataOffset;
            entry.size = block.dataSize;
            entry.repeats = block.numRepeats;
            entry.skip = block.numSkipSamples;
            entry.samples = block.numSamples;
            entry.user_data = block.userData;
            sampler.queue.push_back(entry);
        }
        // Bit 0 announces that more blocks follow; titles end the list with an empty call.
        sampler.complete = (param->flags & 1) == 0;
        if (!sampler.queue.empty()) {
            voice.state_flags &= ~StateFlagEmpty;
        }
        return ORBIS_OK;
    }
    case SamplerReplaceWaveformAddress: {
        const auto* param = As<OrbisNgs2SamplerVoiceWaveformAddressParam>(header);
        if (param == nullptr) {
            return ORBIS_NGS2_ERROR_INVALID_VOICE_CONTROL_SIZE;
        }
        for (auto& block : sampler.queue) {
            if (block.base == param->from) {
                block.data = static_cast<const u8*>(param->to) + (block.data - block.base);
                block.base = static_cast<const u8*>(param->to);
            }
        }
        return ORBIS_OK;
    }
    case SamplerSetWaveformFrameOffset:
        return ORBIS_OK;
    case SamplerExitLoop:
        sampler.exit_loop = true;
        return ORBIS_OK;
    case SamplerSetPitch: {
        const auto* param = As<OrbisNgs2SamplerVoicePitchParam>(header);
        if (param == nullptr) {
            return ORBIS_NGS2_ERROR_INVALID_VOICE_CONTROL_SIZE;
        }
        if (std::isfinite(param->ratio) && param->ratio > 0.0f) {
            sampler.pitch = param->ratio;
        }
        return ORBIS_OK;
    }
    case SamplerSetEnvelope: {
        const auto* param = As<OrbisNgs2SamplerVoiceEnvelopeParam>(header);
        if (param == nullptr) {
            return ORBIS_NGS2_ERROR_INVALID_VOICE_CONTROL_SIZE;
        }
        if (param->aPoint == nullptr && param->numForwardPoints + param->numReleasePoints != 0) {
            return ORBIS_NGS2_ERROR_INVALID_ENVELOPE_POINT_ADDRESS;
        }
        SetEnvelope(sampler.envelope, param->numForwardPoints, param->numReleasePoints,
                    param->aPoint);
        return ORBIS_OK;
    }
    case SamplerSetUserFx: {
        const auto* param = As<OrbisNgs2SamplerVoiceUserFxParam>(header);
        if (param == nullptr) {
            return ORBIS_NGS2_ERROR_INVALID_VOICE_CONTROL_SIZE;
        }
        voice.fx_handler = param->handler;
        voice.fx_user_data = {param->userData0, param->userData1, param->userData2};
        return ORBIS_OK;
    }
    case SamplerSetFilter: {
        const auto* param = As<OrbisNgs2SamplerVoiceFilterParam>(header);
        if (param == nullptr) {
            return ORBIS_NGS2_ERROR_INVALID_VOICE_CONTROL_SIZE;
        }
        if (param->index >= MaxFilters) {
            return ORBIS_NGS2_ERROR_INVALID_FILTER_INDEX;
        }
        SetFilter(voice, param->index, param->type, param->channelMask, &param->param.direct.i0);
        return ORBIS_OK;
    }
    case SamplerSetDistortion:
    case SamplerSetPeakMeter:
        return ORBIS_OK;
    default:
        return ORBIS_NGS2_ERROR_INVALID_VOICE_CONTROL_ID;
    }
}

s32 ApplyBusParam(Voice& voice, const OrbisNgs2VoiceParamHeader* header) {
    const u32 rack_id = voice.rack->rack_id;
    switch (header->id) {
    case SubmixerSetup: {
        const auto* param = As<OrbisNgs2SubmixerVoiceSetupParam>(header);
        if (param == nullptr) {
            return ORBIS_NGS2_ERROR_INVALID_VOICE_CONTROL_SIZE;
        }
        SetupBus(voice, param->numIoChannels);
        return ORBIS_OK;
    }
    case ReverbSetup:
    case CustomSubmixerSetup: {
        // Both start with the number of input and of output channels.
        const auto* param = As<OrbisNgs2ReverbVoiceSetupParam>(header);
        if (param == nullptr) {
            return ORBIS_NGS2_ERROR_INVALID_VOICE_CONTROL_SIZE;
        }
        SetupBus(voice, std::max(param->numInputChannels, param->numOutputChannels));
        return ORBIS_OK;
    }
    case MasteringSetup:
    case CustomMasteringSetup: {
        const auto* param = As<OrbisNgs2MasteringVoiceSetupParam>(header);
        if (param == nullptr) {
            return ORBIS_NGS2_ERROR_INVALID_VOICE_CONTROL_SIZE;
        }
        SetupBus(voice, param->numInputChannels);
        return ORBIS_OK;
    }
    default:
        break;
    }
    if (!voice.setup) {
        return ORBIS_NGS2_ERROR_UNINIT_VOICE;
    }

    switch (header->id) {
    case SubmixerSetUserFx: {
        const auto* param = As<OrbisNgs2SubmixerVoiceUserFxParam>(header);
        if (param == nullptr) {
            return ORBIS_NGS2_ERROR_INVALID_VOICE_CONTROL_SIZE;
        }
        voice.fx_handler = param->handler;
        voice.fx_user_data = {param->userData0, param->userData1, param->userData2};
        voice.fresh = true;
        return ORBIS_OK;
    }
    case SubmixerSetFilter: {
        const auto* param = As<OrbisNgs2SubmixerVoiceFilterParam>(header);
        if (param == nullptr) {
            return ORBIS_NGS2_ERROR_INVALID_VOICE_CONTROL_SIZE;
        }
        if (param->index >= MaxFilters) {
            return ORBIS_NGS2_ERROR_INVALID_FILTER_INDEX;
        }
        SetFilter(voice, param->index, param->type, param->channelMask, &param->param.direct.i0);
        return ORBIS_OK;
    }
    case SubmixerSetEnvelope:
    case SubmixerSetCompressor:
    case SubmixerSetDistortion:
    case SubmixerSetPeakMeter:
        return ORBIS_OK;
    case ReverbSetI3dl2: {
        const auto* param = As<OrbisNgs2ReverbVoiceI3DL2Param>(header);
        if (param == nullptr) {
            return ORBIS_NGS2_ERROR_INVALID_VOICE_CONTROL_SIZE;
        }
        if (voice.reverb) {
            const auto& in = param->i3dl2;
            Dsp::ReverbParams out;
            out.wet = in.wet;
            out.dry = in.dry;
            out.room_mb = static_cast<float>(in.room);
            out.room_hf_mb = static_cast<float>(in.roomHF);
            out.decay_time = in.decayTime;
            out.decay_hf_ratio = in.decayHFRatio;
            out.reflections_mb = static_cast<float>(in.reflections);
            out.reflections_delay = in.reflectionsDelay;
            out.reverb_mb = static_cast<float>(in.reverb);
            out.reverb_delay = in.reverbDelay;
            out.diffusion = in.diffusion;
            out.density = in.density;
            out.hf_reference = in.HFReference;
            voice.reverb->SetParams(out);
            voice.tail_blocks = TailBlocks(*voice.rack->system,
                                           std::clamp(in.decayTime, 0.1f, 20.0f) * 1.5f + 0.5f);
        }
        return ORBIS_OK;
    }
    case MasteringSetLimiter: {
        const auto* param = As<OrbisNgs2MasteringVoiceLimiterParam>(header);
        if (param == nullptr) {
            return ORBIS_NGS2_ERROR_INVALID_VOICE_CONTROL_SIZE;
        }
        voice.limiter_enabled = param->enableFlag != 0;
        if (std::isfinite(param->threshold) && param->threshold > 0.0f) {
            voice.limiter_threshold = param->threshold;
        }
        return ORBIS_OK;
    }
    case MasteringSetGain: {
        const auto* param = As<OrbisNgs2MasteringVoiceGainParam>(header);
        if (param == nullptr) {
            return ORBIS_NGS2_ERROR_INVALID_VOICE_CONTROL_SIZE;
        }
        voice.gain_full_band = std::isfinite(param->fbwLevel) ? param->fbwLevel : 1.0f;
        voice.gain_lfe = std::isfinite(param->lfeLevel) ? param->lfeLevel : 1.0f;
        return ORBIS_OK;
    }
    case MasteringSetOutput: {
        const auto* param = As<OrbisNgs2MasteringVoiceOutputParam>(header);
        if (param == nullptr) {
            return ORBIS_NGS2_ERROR_INVALID_VOICE_CONTROL_SIZE;
        }
        voice.output_id = param->outputId;
        return ORBIS_OK;
    }
    case MasteringSetMatrix:
    case MasteringSetLfe:
    case MasteringSetPeakMeter:
        return ORBIS_OK;
    default:
        break;
    }

    if (rack_id == RackIdCustomSubmixer &&
        (header->id & CustomModuleParamMask) == CustomModuleParamBase) {
        const u32 module = (header->id >> 8) & 0xff;
        if (module == ModuleChorus && voice.chorus) {
            if (const auto* param = As<OrbisNgs2CustomVoiceChorusParam>(header)) {
                Dsp::ChorusParams out;
                out.num_phases = param->numPhases;
                out.input_level = param->inputLevel;
                out.delay_ms = param->delayTime;
                out.modulation_rate = param->modulationRatio;
                out.modulation_depth = param->modulationDepth;
                out.feedback = param->feedbackLevel;
                out.wet = param->wetLevel;
                out.dry = param->dryLevel;
                voice.chorus->SetParams(out);
            }
        } else if (module == ModuleDelay && voice.delay) {
            if (const auto* param = As<OrbisNgs2CustomVoiceDelayParam>(header)) {
                Dsp::DelayParams out;
                out.dry = param->dryLevel;
                out.wet = param->wetLevel;
                out.input_level = param->inputLevel;
                out.feedback = param->feedbackLevel;
                out.lowpass_fc = param->lowpassFc;
                out.num_taps = std::min<u32>(param->numTaps, Dsp::DelayParams::MaxTaps);
                for (u32 i = 0; i < out.num_taps; ++i) {
                    out.tap_level[i] = param->aTap[i].tapLevel;
                    out.tap_ms[i] = param->aTap[i].delayTime;
                }
                voice.delay->SetParams(out);
            }
        } else if (module == ModulePitchShift && voice.pitch_shift) {
            if (const auto* param = As<OrbisNgs2CustomVoicePitchShiftParam>(header)) {
                voice.pitch_shift->SetCents(param->cent);
            }
        }
        return ORBIS_OK;
    }
    return ORBIS_NGS2_ERROR_INVALID_VOICE_CONTROL_ID;
}

/// Unpatches everything that feeds a rack that is going away.
void DetachRack(System& system, const Rack& rack) {
    for (const auto& other : system.racks) {
        for (const auto& voice : other->voices) {
            for (auto& port : voice->ports) {
                if (port.dest != nullptr && port.dest->rack == &rack) {
                    port.dest = nullptr;
                    port.dest_handle = 0;
                }
            }
        }
    }
    system.order.clear();
    system.order_dirty = true;
}

void ReleaseRack(Registry& registry, Rack& rack) {
    for (const auto& voice : rack.voices) {
        registry.handles.erase(reinterpret_cast<OrbisNgs2Handle>(voice.get()));
    }
    registry.handles.erase(reinterpret_cast<OrbisNgs2Handle>(&rack));
}

} // namespace

s32 CallBufferHandler(OrbisNgs2BufferAllocHandler handler, OrbisNgs2ContextBufferInfo* info) {
#ifdef SHADPS4_ENABLE_FEX_GUEST_CPU
    const void* address = reinterpret_cast<const void*>(handler);
    if (Core::GuestCpu::IsGuestFunctionAddress(address)) {
        return static_cast<s32>(
            Core::GuestCpu::RunGuestFunctionOrAbort(address, "Ngs2 buffer handler", info));
    }
#endif
    return handler(info);
}

// --- systems ------------------------------------------------------------------------------------

s32 SystemCreate(const OrbisNgs2SystemOption* option, const OrbisNgs2ContextBufferInfo& buffer,
                 OrbisNgs2BufferFreeHandler free_handler, OrbisNgs2Handle* out_handle) {
    auto& registry = GetRegistry();
    std::scoped_lock lock{registry.mutex};

    auto system = std::make_unique<System>();
    system->serial = registry.next_serial++;
    system->buffer = buffer;
    system->free_handler = free_handler;
    if (option != nullptr) {
        system->option = *option;
        system->sample_rate = option->sampleRate;
        system->grain_samples = option->numGrainSamples;
    }
    char name[ORBIS_NGS2_SYSTEM_NAME_LENGTH + 1]{};
    std::memcpy(name, system->option.name, ORBIS_NGS2_SYSTEM_NAME_LENGTH);
    GetTrace().Line("SYS create S%u name='%s' flags=%x maxGrain=%u grain=%u rate=%u", system->serial,
                    name, system->option.flags, system->option.maxGrainSamples,
                    system->grain_samples, system->sample_rate);
    LOG_INFO(Lib_Ngs2, "system '{}' created: {} Hz, {} samples per render", name,
             system->sample_rate, system->grain_samples);

    const auto handle = reinterpret_cast<OrbisNgs2Handle>(system.get());
    registry.handles.emplace(handle, HandleType::System);
    registry.systems.push_back(std::move(system));
    *out_handle = handle;
    return ORBIS_OK;
}

s32 SystemDestroy(OrbisNgs2Handle handle, OrbisNgs2ContextBufferInfo* out_buffer) {
    auto& registry = GetRegistry();
    std::unique_lock lock{registry.mutex};
    auto* system = Lookup<System>(registry, handle, HandleType::System);
    if (system == nullptr) {
        return ORBIS_NGS2_ERROR_INVALID_SYSTEM_HANDLE;
    }
    GetTrace().Line("SYS destroy S%u", system->serial);

    std::vector<std::pair<OrbisNgs2BufferFreeHandler, OrbisNgs2ContextBufferInfo>> to_free;
    for (const auto& rack : system->racks) {
        ReleaseRack(registry, *rack);
        if (rack->free_handler != nullptr) {
            to_free.emplace_back(rack->free_handler, rack->buffer);
        }
    }
    if (out_buffer != nullptr) {
        *out_buffer = system->buffer;
    }
    if (system->free_handler != nullptr) {
        to_free.emplace_back(system->free_handler, system->buffer);
    }
    registry.handles.erase(handle);
    std::erase_if(registry.systems, [&](const auto& entry) { return entry.get() == system; });
    lock.unlock();

    for (auto& [handler, info] : to_free) {
        CallBufferHandler(handler, &info);
    }
    return ORBIS_OK;
}

s32 SystemRender(OrbisNgs2Handle handle, const OrbisNgs2RenderBufferInfo* buffers, u32 count) {
    auto& registry = GetRegistry();
    std::scoped_lock lock{registry.mutex};
    auto* system = Lookup<System>(registry, handle, HandleType::System);
    if (system == nullptr) {
        return ORBIS_NGS2_ERROR_INVALID_SYSTEM_HANDLE;
    }
    if (buffers == nullptr && count != 0) {
        return ORBIS_NGS2_ERROR_INVALID_BUFFER_ADDRESS;
    }
    auto& trace = GetTrace();
    if (trace.Enabled() && (system->render_count < 4 || system->render_count % 1024 == 0)) {
        std::string text;
        char entry[96];
        for (u32 i = 0; i < count; ++i) {
            std::snprintf(entry, sizeof(entry), " [%u]{%p size=%zu type=%x ch=%u}", i,
                          buffers[i].buffer, buffers[i].bufferSize, buffers[i].waveformType,
                          buffers[i].numChannels);
            text += entry;
        }
        trace.Line("RENDER S%u #%llu n=%u%s", system->serial,
                   static_cast<unsigned long long>(system->render_count), count, text.c_str());
    }
    ++system->render_count;
    if (!system->dumps_opened) {
        OpenDumps(*system, count, buffers);
    }

    for (u32 i = 0; i < count; ++i) {
        if (buffers[i].buffer != nullptr) {
            std::memset(buffers[i].buffer, 0, buffers[i].bufferSize);
        }
    }
    const u32 grain = std::clamp<u32>(system->grain_samples, 64, MaxGrain);
    if (system->order_dirty) {
        RebuildOrder(*system);
    }

    for (const auto& rack : system->racks) {
        if (!rack->IsSampler()) {
            continue;
        }
        for (const auto& voice : rack->voices) {
            if (voice->playing && !voice->paused && voice->sampler) {
                RenderSampler(*system, *voice, grain);
            }
        }
    }
    for (Voice* voice : system->order) {
        if (voice->playing && !voice->paused && voice->input) {
            RenderBus(*system, *voice, buffers, count, grain);
        } else if (voice->input && voice->has_input) {
            std::fill_n(voice->input.get(), size_t{MaxChannels} * MaxGrain, 0.0f);
            voice->has_input = false;
        }
    }
    return ORBIS_OK;
}

s32 SystemSetGrainSamples(OrbisNgs2Handle handle, u32 samples) {
    auto& registry = GetRegistry();
    std::scoped_lock lock{registry.mutex};
    auto* system = Lookup<System>(registry, handle, HandleType::System);
    if (system == nullptr) {
        return ORBIS_NGS2_ERROR_INVALID_SYSTEM_HANDLE;
    }
    if (samples < 64 || samples > MaxGrain || (samples & 63) != 0) {
        return ORBIS_NGS2_ERROR_INVALID_NUM_GRAIN_SAMPLES;
    }
    GetTrace().Line("SYS S%u grain=%u", system->serial, samples);
    system->grain_samples = samples;
    return ORBIS_OK;
}

s32 SystemSetSampleRate(OrbisNgs2Handle handle, u32 sample_rate) {
    auto& registry = GetRegistry();
    std::scoped_lock lock{registry.mutex};
    auto* system = Lookup<System>(registry, handle, HandleType::System);
    if (system == nullptr) {
        return ORBIS_NGS2_ERROR_INVALID_SYSTEM_HANDLE;
    }
    if (sample_rate < 8000 || sample_rate > 192000) {
        return ORBIS_NGS2_ERROR_INVALID_SAMPLE_RATE;
    }
    GetTrace().Line("SYS S%u rate=%u", system->serial, sample_rate);
    system->sample_rate = sample_rate;
    return ORBIS_OK;
}

s32 SystemLock(OrbisNgs2Handle handle) {
    auto& registry = GetRegistry();
    {
        std::scoped_lock lock{registry.mutex};
        if (Lookup<System>(registry, handle, HandleType::System) == nullptr) {
            return ORBIS_NGS2_ERROR_INVALID_SYSTEM_HANDLE;
        }
    }
    registry.mutex.lock();
    return ORBIS_OK;
}

s32 SystemUnlock(OrbisNgs2Handle handle) {
    auto& registry = GetRegistry();
    registry.mutex.unlock();
    return ORBIS_OK;
}

s32 SystemSetUserData(OrbisNgs2Handle handle, uintptr_t user_data) {
    auto& registry = GetRegistry();
    std::scoped_lock lock{registry.mutex};
    auto* system = Lookup<System>(registry, handle, HandleType::System);
    if (system == nullptr) {
        return ORBIS_NGS2_ERROR_INVALID_SYSTEM_HANDLE;
    }
    system->user_data = user_data;
    return ORBIS_OK;
}

s32 SystemGetUserData(OrbisNgs2Handle handle, uintptr_t* out_user_data) {
    auto& registry = GetRegistry();
    std::scoped_lock lock{registry.mutex};
    auto* system = Lookup<System>(registry, handle, HandleType::System);
    if (system == nullptr) {
        return ORBIS_NGS2_ERROR_INVALID_SYSTEM_HANDLE;
    }
    if (out_user_data == nullptr) {
        return ORBIS_NGS2_ERROR_INVALID_OUT_ADDRESS;
    }
    *out_user_data = system->user_data;
    return ORBIS_OK;
}

s32 SystemGetInfo(OrbisNgs2Handle handle, OrbisNgs2SystemInfo* out_info, size_t size) {
    auto& registry = GetRegistry();
    std::scoped_lock lock{registry.mutex};
    auto* system = Lookup<System>(registry, handle, HandleType::System);
    if (system == nullptr) {
        return ORBIS_NGS2_ERROR_INVALID_SYSTEM_HANDLE;
    }
    if (out_info == nullptr) {
        return ORBIS_NGS2_ERROR_INVALID_OUT_ADDRESS;
    }
    OrbisNgs2SystemInfo info{};
    std::memcpy(info.name, system->option.name, sizeof(info.name));
    info.systemHandle = handle;
    info.bufferInfo = system->buffer;
    info.uid = system->serial;
    info.minGrainSamples = 64;
    info.maxGrainSamples = system->option.maxGrainSamples != 0 ? system->option.maxGrainSamples
                                                                 : 512;
    info.rackCount = static_cast<u32>(system->racks.size());
    info.renderCount = static_cast<s64>(system->render_count);
    info.sampleRate = system->sample_rate;
    info.numGrainSamples = system->grain_samples;
    std::memcpy(out_info, &info, std::min(size, sizeof(info)));
    return ORBIS_OK;
}

s32 SystemEnumRackHandles(OrbisNgs2Handle handle, OrbisNgs2Handle* out_handles, u32 max_handles) {
    auto& registry = GetRegistry();
    std::scoped_lock lock{registry.mutex};
    auto* system = Lookup<System>(registry, handle, HandleType::System);
    if (system == nullptr) {
        return ORBIS_NGS2_ERROR_INVALID_SYSTEM_HANDLE;
    }
    u32 count = 0;
    for (const auto& rack : system->racks) {
        if (out_handles != nullptr && count < max_handles) {
            out_handles[count] = reinterpret_cast<OrbisNgs2Handle>(rack.get());
        }
        ++count;
    }
    return static_cast<s32>(std::min(count, max_handles));
}

// --- racks --------------------------------------------------------------------------------------

s32 RackQueryBufferSize(u32 rack_id, const OrbisNgs2RackOption* option,
                        OrbisNgs2ContextBufferInfo* out_buffer) {
    if (out_buffer == nullptr) {
        return ORBIS_NGS2_ERROR_INVALID_OUT_ADDRESS;
    }
    TraceRackOption("RACK query", rack_id, option);
    // Racks live in the emulator's own memory. The title still gets to allocate something, it
    // expects the buffer back when the rack goes away.
    out_buffer->hostBuffer = nullptr;
    out_buffer->hostBufferSize = 0x1000;
    std::memset(out_buffer->reserved, 0, sizeof(out_buffer->reserved));
    return ORBIS_OK;
}

s32 RackCreate(OrbisNgs2Handle system_handle, u32 rack_id, const OrbisNgs2RackOption* option,
               const OrbisNgs2ContextBufferInfo& buffer, OrbisNgs2BufferFreeHandler free_handler,
               OrbisNgs2Handle* out_handle) {
    auto& registry = GetRegistry();
    std::scoped_lock lock{registry.mutex};
    auto* system = Lookup<System>(registry, system_handle, HandleType::System);
    if (system == nullptr) {
        return ORBIS_NGS2_ERROR_INVALID_SYSTEM_HANDLE;
    }
    if (out_handle == nullptr) {
        return ORBIS_NGS2_ERROR_INVALID_OUT_ADDRESS;
    }
    switch (rack_id) {
    case RackIdSampler:
    case RackIdSubmixer:
    case RackIdReverb:
    case RackIdEqualizer:
    case RackIdMastering:
    case RackIdCustomSampler:
    case RackIdCustomSubmixer:
    case RackIdCustomMastering:
        break;
    default:
        LOG_ERROR(Lib_Ngs2, "unknown rack type {:#x}", rack_id);
        return ORBIS_NGS2_ERROR_INVALID_RACK_ID;
    }

    auto rack = std::make_unique<Rack>();
    rack->system = system;
    rack->rack_id = rack_id;
    rack->serial = registry.next_serial++;
    rack->buffer = buffer;
    rack->free_handler = free_handler;
    if (option != nullptr) {
        std::memcpy(&rack->option, option, std::min<size_t>(option->size, sizeof(rack->option)));
        if ((rack_id & 0xf000) == 0x4000 && option->size >= sizeof(OrbisNgs2CustomRackOption)) {
            const auto* custom = reinterpret_cast<const OrbisNgs2CustomRackOption*>(option);
            for (u32 i = 0; i < custom->numModules && i < ORBIS_NGS2_CUSTOM_MAX_MODULES; ++i) {
                ModuleInfo module;
                module.id = custom->aModule[i].moduleId;
                const auto* module_option = custom->aModule[i].option;
                if (module.id == ModuleDelay && module_option != nullptr &&
                    module_option->size >= sizeof(OrbisNgs2CustomDelayModuleOption)) {
                    module.delay_max_ms =
                        reinterpret_cast<const OrbisNgs2CustomDelayModuleOption*>(module_option)
                            ->maxLength;
                }
                rack->modules.push_back(module);
            }
        }
    }
    const u32 max_voices =
        option != nullptr && option->maxVoices != 0 ? option->maxVoices : DefaultMaxVoices(rack_id);
    const u32 max_ports = std::clamp<u32>(rack->option.maxPorts, 1, 16);
    const u32 max_matrices = std::clamp<u32>(rack->option.maxMatrices, 1, 16);
    rack->voices.reserve(max_voices);
    for (u32 i = 0; i < max_voices; ++i) {
        auto voice = std::make_unique<Voice>();
        voice->rack = rack.get();
        voice->index = i;
        voice->ports.resize(max_ports);
        for (auto& port : voice->ports) {
            port.gain.fill(-1.0f);
        }
        voice->matrices.resize(max_matrices);
        registry.handles.emplace(reinterpret_cast<OrbisNgs2Handle>(voice.get()), HandleType::Voice);
        rack->voices.push_back(std::move(voice));
    }

    char what[96];
    std::snprintf(what, sizeof(what), "RACK create R%u sys=S%u handle=%p", rack->serial,
                  system->serial, static_cast<void*>(rack.get()));
    TraceRackOption(what, rack_id, option);

    const auto handle = reinterpret_cast<OrbisNgs2Handle>(rack.get());
    registry.handles.emplace(handle, HandleType::Rack);
    system->racks.push_back(std::move(rack));
    system->order_dirty = true;
    *out_handle = handle;
    return ORBIS_OK;
}

s32 RackDestroy(OrbisNgs2Handle handle, OrbisNgs2ContextBufferInfo* out_buffer) {
    auto& registry = GetRegistry();
    std::unique_lock lock{registry.mutex};
    auto* rack = Lookup<Rack>(registry, handle, HandleType::Rack);
    if (rack == nullptr) {
        return ORBIS_NGS2_ERROR_INVALID_RACK_HANDLE;
    }
    GetTrace().Line("RACK destroy R%u", rack->serial);
    System& system = *rack->system;
    DetachRack(system, *rack);
    ReleaseRack(registry, *rack);
    OrbisNgs2ContextBufferInfo buffer = rack->buffer;
    const auto free_handler = rack->free_handler;
    if (out_buffer != nullptr) {
        *out_buffer = buffer;
    }
    std::erase_if(system.racks, [&](const auto& entry) { return entry.get() == rack; });
    lock.unlock();

    if (free_handler != nullptr) {
        CallBufferHandler(free_handler, &buffer);
    }
    return ORBIS_OK;
}

s32 RackGetVoiceHandle(OrbisNgs2Handle handle, u32 voice_index, OrbisNgs2Handle* out_handle) {
    auto& registry = GetRegistry();
    std::scoped_lock lock{registry.mutex};
    auto* rack = Lookup<Rack>(registry, handle, HandleType::Rack);
    if (rack == nullptr) {
        return ORBIS_NGS2_ERROR_INVALID_RACK_HANDLE;
    }
    if (out_handle == nullptr) {
        return ORBIS_NGS2_ERROR_INVALID_OUT_ADDRESS;
    }
    if (voice_index >= rack->voices.size()) {
        return ORBIS_NGS2_ERROR_INVALID_VOICE_INDEX;
    }
    *out_handle = reinterpret_cast<OrbisNgs2Handle>(rack->voices[voice_index].get());
    return ORBIS_OK;
}

s32 RackGetInfo(OrbisNgs2Handle handle, OrbisNgs2RackInfo* out_info, size_t size) {
    auto& registry = GetRegistry();
    std::scoped_lock lock{registry.mutex};
    auto* rack = Lookup<Rack>(registry, handle, HandleType::Rack);
    if (rack == nullptr) {
        return ORBIS_NGS2_ERROR_INVALID_RACK_HANDLE;
    }
    if (out_info == nullptr) {
        return ORBIS_NGS2_ERROR_INVALID_OUT_ADDRESS;
    }
    OrbisNgs2RackInfo info{};
    std::memcpy(info.name, rack->option.name, sizeof(info.name));
    info.rackHandle = handle;
    info.bufferInfo = rack->buffer;
    info.ownerSystemHandle = reinterpret_cast<OrbisNgs2Handle>(rack->system);
    info.rackId = rack->rack_id;
    info.uid = rack->serial;
    info.minGrainSamples = 64;
    info.maxGrainSamples = rack->option.maxGrainSamples;
    info.maxVoices = static_cast<u32>(rack->voices.size());
    info.maxMatrices = rack->option.maxMatrices;
    info.maxPorts = rack->option.maxPorts;
    for (const auto& voice : rack->voices) {
        info.activeVoiceCount += voice->playing ? 1 : 0;
    }
    std::memcpy(out_info, &info, std::min(size, sizeof(info)));
    return ORBIS_OK;
}

s32 RackSetUserData(OrbisNgs2Handle handle, uintptr_t user_data) {
    auto& registry = GetRegistry();
    std::scoped_lock lock{registry.mutex};
    auto* rack = Lookup<Rack>(registry, handle, HandleType::Rack);
    if (rack == nullptr) {
        return ORBIS_NGS2_ERROR_INVALID_RACK_HANDLE;
    }
    rack->user_data = user_data;
    return ORBIS_OK;
}

s32 RackGetUserData(OrbisNgs2Handle handle, uintptr_t* out_user_data) {
    auto& registry = GetRegistry();
    std::scoped_lock lock{registry.mutex};
    auto* rack = Lookup<Rack>(registry, handle, HandleType::Rack);
    if (rack == nullptr) {
        return ORBIS_NGS2_ERROR_INVALID_RACK_HANDLE;
    }
    if (out_user_data == nullptr) {
        return ORBIS_NGS2_ERROR_INVALID_OUT_ADDRESS;
    }
    *out_user_data = rack->user_data;
    return ORBIS_OK;
}

// --- voices -------------------------------------------------------------------------------------

s32 VoiceControl(OrbisNgs2Handle handle, const OrbisNgs2VoiceParamHeader* params) {
    auto& registry = GetRegistry();
    std::scoped_lock lock{registry.mutex};
    auto* voice = Lookup<Voice>(registry, handle, HandleType::Voice);
    if (voice == nullptr) {
        GetTrace().Line("CTL invalid voice %llx", static_cast<unsigned long long>(handle));
        return ORBIS_NGS2_ERROR_INVALID_VOICE_HANDLE;
    }
    if (params == nullptr) {
        return ORBIS_NGS2_ERROR_INVALID_VOICE_CONTROL_ADDRESS;
    }

    s32 result = ORBIS_OK;
    const auto* header = params;
    for (u32 guard = 0; guard < 1024; ++guard) {
        if (header->size < sizeof(OrbisNgs2VoiceParamHeader)) {
            return ORBIS_NGS2_ERROR_INVALID_VOICE_CONTROL_SIZE;
        }
        TraceParam(*voice, header);

        bool handled = false;
        s32 status = ApplyCommonParam(registry, *voice, header, handled);
        if (!handled) {
            status = voice->rack->IsSampler() ? ApplySamplerParam(*voice, header)
                                              : ApplyBusParam(*voice, header);
        }
        if (status < 0) {
            GetTrace().Line("    -> %08x", static_cast<u32>(status));
            static u32 reported = 0;
            if (reported < 32) {
                ++reported;
                LOG_WARNING(Lib_Ngs2, "voice parameter {:#x} (size {}) on a {:#x} rack: {:#x}",
                            header->id, header->size, voice->rack->rack_id,
                            static_cast<u32>(status));
            }
            if (result == ORBIS_OK) {
                result = status;
            }
        }

        if (header->next == 0) {
            break;
        }
        header = reinterpret_cast<const OrbisNgs2VoiceParamHeader*>(
            reinterpret_cast<const u8*>(header) + header->next);
    }
    return result;
}

s32 VoiceGetState(OrbisNgs2Handle handle, OrbisNgs2VoiceState* out_state, size_t size) {
    auto& registry = GetRegistry();
    std::scoped_lock lock{registry.mutex};
    auto* voice = Lookup<Voice>(registry, handle, HandleType::Voice);
    if (voice == nullptr) {
        return ORBIS_NGS2_ERROR_INVALID_VOICE_HANDLE;
    }
    if (out_state == nullptr) {
        return ORBIS_NGS2_ERROR_INVALID_OUT_ADDRESS;
    }
    std::memset(out_state, 0, size);

    if (voice->sampler) {
        OrbisNgs2SamplerVoiceState state{};
        state.voiceState.stateFlags = voice->state_flags;
        state.envelopeHeight = voice->sampler->envelope.height;
        state.peakHeight = voice->peak;
        state.numDecodedSamples = voice->sampler->decoded_samples;
        state.decodedDataSize = voice->sampler->decoded_bytes;
        state.userData = voice->sampler->user_data;
        state.waveformData = voice->sampler->waveform_data;
        std::memcpy(out_state, &state, std::min(size, sizeof(state)));
    } else if (voice->rack->IsMastering()) {
        OrbisNgs2MasteringVoiceState state{};
        state.voiceState.stateFlags = voice->state_flags;
        state.limiterPeakLevel = voice->peak;
        state.limiterPressLevel = voice->limiter.Gain();
        std::fill(std::begin(state.aInputPeakHeight), std::end(state.aInputPeakHeight),
                  voice->peak);
        std::fill(std::begin(state.aOutputPeakHeight), std::end(state.aOutputPeakHeight),
                  voice->peak);
        std::memcpy(out_state, &state, std::min(size, sizeof(state)));
    } else {
        OrbisNgs2SubmixerVoiceState state{};
        state.voiceState.stateFlags = voice->state_flags;
        state.envelopeHeight = 1.0f;
        state.peakHeight = voice->peak;
        state.compressorHeight = 1.0f;
        std::memcpy(out_state, &state, std::min(size, sizeof(state)));
    }
    return ORBIS_OK;
}

s32 VoiceGetStateFlags(OrbisNgs2Handle handle, u32* out_flags) {
    auto& registry = GetRegistry();
    std::scoped_lock lock{registry.mutex};
    auto* voice = Lookup<Voice>(registry, handle, HandleType::Voice);
    if (voice == nullptr) {
        return ORBIS_NGS2_ERROR_INVALID_VOICE_HANDLE;
    }
    if (out_flags == nullptr) {
        return ORBIS_NGS2_ERROR_INVALID_OUT_ADDRESS;
    }
    *out_flags = voice->state_flags;
    return ORBIS_OK;
}

s32 VoiceGetOwner(OrbisNgs2Handle handle, OrbisNgs2Handle* out_rack, u32* out_index) {
    auto& registry = GetRegistry();
    std::scoped_lock lock{registry.mutex};
    auto* voice = Lookup<Voice>(registry, handle, HandleType::Voice);
    if (voice == nullptr) {
        return ORBIS_NGS2_ERROR_INVALID_VOICE_HANDLE;
    }
    if (out_rack != nullptr) {
        *out_rack = reinterpret_cast<OrbisNgs2Handle>(voice->rack);
    }
    if (out_index != nullptr) {
        *out_index = voice->index;
    }
    return ORBIS_OK;
}

s32 VoiceGetPortInfo(OrbisNgs2Handle handle, u32 port_index, OrbisNgs2VoicePortInfo* out_info,
                     size_t size) {
    auto& registry = GetRegistry();
    std::scoped_lock lock{registry.mutex};
    auto* voice = Lookup<Voice>(registry, handle, HandleType::Voice);
    if (voice == nullptr) {
        return ORBIS_NGS2_ERROR_INVALID_VOICE_HANDLE;
    }
    if (out_info == nullptr) {
        return ORBIS_NGS2_ERROR_INVALID_OUT_ADDRESS;
    }
    if (port_index >= voice->ports.size()) {
        return ORBIS_NGS2_ERROR_INVALID_PORT_INDEX;
    }
    const auto& port = voice->ports[port_index];
    OrbisNgs2VoicePortInfo info{};
    info.matrixId = port.matrix;
    info.volume = port.volume;
    info.destInputId = port.dest_input;
    info.destHandle = port.dest_handle;
    std::memcpy(out_info, &info, std::min(size, sizeof(info)));
    return ORBIS_OK;
}

s32 VoiceGetMatrixInfo(OrbisNgs2Handle handle, u32 matrix_index,
                       OrbisNgs2VoiceMatrixInfo* out_info, size_t size) {
    auto& registry = GetRegistry();
    std::scoped_lock lock{registry.mutex};
    auto* voice = Lookup<Voice>(registry, handle, HandleType::Voice);
    if (voice == nullptr) {
        return ORBIS_NGS2_ERROR_INVALID_VOICE_HANDLE;
    }
    if (out_info == nullptr) {
        return ORBIS_NGS2_ERROR_INVALID_OUT_ADDRESS;
    }
    if (matrix_index >= voice->matrices.size()) {
        return ORBIS_NGS2_ERROR_INVALID_MATRIX_INDEX;
    }
    const auto& matrix = voice->matrices[matrix_index];
    OrbisNgs2VoiceMatrixInfo info{};
    info.numLevels = matrix.count;
    std::copy_n(matrix.levels.begin(), matrix.count, info.aLevel);
    std::memcpy(out_info, &info, std::min(size, sizeof(info)));
    return ORBIS_OK;
}

} // namespace Libraries::Ngs2::Engine
