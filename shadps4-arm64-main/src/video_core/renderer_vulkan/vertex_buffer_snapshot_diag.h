// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier-Identifier: GPL-2.0-or-later

#pragma once

#include <bitset>
#include <cmath>
#include <span>
#include <unordered_map>

#include "common/logging/log.h"
#include "common/types.h"
#include "video_core/amdgpu/resource.h"

namespace Vulkan {

/// Draw-time snapshot of guest vertex/index buffers for the DSR Adreno 830
/// walk-triggered mesh explosion (docs/dsr-adreno830-vertex-explosion.md).
/// The patched-shader experiments falsified the payload-SSBO VS family; the
/// remaining suspects read vs_in_attr inputs, so the garbage (if guest-side)
/// must travel through the vertex buffer descriptors bound by the fetch
/// shader. Logs descriptor changes and float sanity of the guest bytes so an
/// idle-vs-walk diff points at the corrupted stream. Gated on
/// gpu.dump_shaders; remove once the culprit is identified.
class VertexBufferSnapshotDiag {
public:
    struct BufferStats {
        u32 nan_count{};
        u32 inf_count{};
        u32 huge_count{};
        float max_abs{};
        u32 head0{};
        u32 head1{};
    };

    void LogDraw(u64 vs_hash, u32 num_indices, std::span<const AmdGpu::Buffer> guest_buffers,
                 bool can_read_guest) {
        if (lines_written >= kMaxLines) {
            return;
        }
        auto& state = per_vs[vs_hash];
        ++state.draws;

        u64 desc_sig = 0x9E3779B97F4A7C15ULL;
        for (const auto& buffer : guest_buffers) {
            desc_sig = desc_sig * 0x100000001B3ULL ^ buffer.base_address;
            desc_sig = desc_sig * 0x100000001B3ULL ^ buffer.GetSize();
            desc_sig = desc_sig * 0x100000001B3ULL ^ buffer.GetStride();
        }

        std::span<const u32> words[MaxSampledBuffers]{};
        std::span<const u32> words_mid[MaxSampledBuffers]{};
        u32 num_sampled{};
        if (can_read_guest) {
            for (const auto& buffer : guest_buffers) {
                if (num_sampled >= MaxSampledBuffers) {
                    break;
                }
                if (buffer.GetSize() == 0) {
                    continue;
                }
                words[num_sampled] = SampleWords(buffer.base_address, buffer.GetSize(), 0);
                words_mid[num_sampled] = SampleWords(buffer.base_address, buffer.GetSize(),
                                                     buffer.GetSize() / 2);
                ++num_sampled;
            }
        }

        // Decide whether this draw is worth a log line: descriptor change,
        // stats-class change, first draw, or a heartbeat.
        bool should_log = state.draws == 1 || desc_sig != state.desc_sig || state.heartbeat == 0;
        u64 stats_sig = 0xC2B2AE3D27D4EB4FULL;
        if (can_read_guest) {
            for (u32 i = 0; i < num_sampled; ++i) {
                const auto a = Classify(words[i]);
                const auto b = Classify(words_mid[i]);
                stats_sig = stats_sig * 0x100000001B3ULL ^ Bucket(a);
                stats_sig = stats_sig * 0x100000001B3ULL ^ Bucket(b);
            }
        }
        should_log = should_log || stats_sig != state.stats_sig;
        if (!should_log) {
            --state.heartbeat;
            return;
        }
        state.desc_sig = desc_sig;
        state.stats_sig = stats_sig;
        state.heartbeat = kHeartbeatDraws;

        u32 out_i{};
        for (const auto& buffer : guest_buffers) {
            if (out_i >= MaxSampledBuffers) {
                break;
            }
            std::string extra = "unreadable";
            if (can_read_guest && buffer.GetSize() > 0) {
                const auto a = Classify(words[out_i]);
                const auto b = Classify(words_mid[out_i]);
                extra = fmt::format("nan={} inf={} huge={} max={:.3e} mid_nan={} mid_huge={} "
                                    "head={:08x} {:08x}",
                                    a.nan_count, a.inf_count, a.huge_count, a.max_abs,
                                    b.nan_count, b.huge_count, a.head0, a.head1);
            }
            LOG_WARNING(Render_Vulkan,
                        "VBSNAP vs={:#x} draws={} nidx={} buf={} addr={:#x} size={} stride={} {}",
                        vs_hash, state.draws, num_indices, out_i,
                        static_cast<u64>(buffer.base_address), buffer.GetSize(),
                        buffer.GetStride(), extra);
            ++lines_written;
            ++out_i;
        }
    }

    void LogIndexBuffer(u64 vs_hash, bool is_index16, VAddr address, u32 num_indices,
                        bool can_read_guest) {
        if (lines_written >= kMaxLines) {
            return;
        }
        auto& state = per_vs[vs_hash];
        ++state.index_draws;
        const u32 index_size = is_index16 ? sizeof(u16) : sizeof(u32);
        u32 max_index{};
        u32 over_threshold{};
        if (can_read_guest) {
            const auto* indices16 = std::bit_cast<const u16*>(address);
            const auto* indices32 = std::bit_cast<const u32*>(address);
            const u32 count = std::min(num_indices, kIndexSampleCount);
            for (u32 i = 0; i < count; ++i) {
                const u32 index = is_index16 ? u32(indices16[i]) : indices32[i];
                max_index = std::max(max_index, index);
                if (index > kIndexSanityLimit) {
                    ++over_threshold;
                }
            }
        }
        const u64 sig = address * 0x100000001B3ULL ^ num_indices ^ (max_index >> 8) ^
                        (over_threshold >> 2);
        if (state.index_draws != 1 && sig == state.index_sig) {
            return;
        }
        state.index_sig = sig;
        LOG_WARNING(Render_Vulkan,
                    "IBSNAP vs={:#x} idraws={} n={} i16={} addr={:#x} max_idx={} over={} {}",
                    vs_hash, state.index_draws, num_indices, is_index16 ? 1 : 0, address, max_index,
                    over_threshold, can_read_guest ? "" : "unreadable");
        ++lines_written;
    }

private:
    static constexpr u32 kMaxLines = 700;
    static constexpr u32 kHeartbeatDraws = 600;
    static constexpr u32 kSampleBytes = 2048;
    static constexpr u32 kMaxSampledBuffers = 16;
    static constexpr u32 kIndexSampleCount = 2048;
    static constexpr u32 kIndexSanityLimit = 1u << 20;

    struct HashState {
        u32 draws{};
        u32 index_draws{};
        u32 heartbeat{kHeartbeatDraws};
        u64 desc_sig{};
        u64 stats_sig{};
        u64 index_sig{};
    };

    std::unordered_map<u64, HashState> per_vs;
    u32 lines_written{};

    static std::span<const u32> SampleWords(VAddr address, u32 size, u32 offset) {
        const u32 begin = std::min(offset, size & ~3u);
        const u32 bytes = std::min(kSampleBytes, size - begin) & ~3u;
        if (bytes == 0) {
            return {};
        }
        return {std::bit_cast<const u32*>(address + begin), bytes / sizeof(u32)};
    }

    static BufferStats Classify(std::span<const u32> dwords) {
        BufferStats stats{};
        for (const u32 raw : dwords) {
            const float f = std::bit_cast<float>(raw);
            if (std::isnan(f)) {
                ++stats.nan_count;
                continue;
            }
            if (std::isinf(f)) {
                ++stats.inf_count;
                continue;
            }
            const float abs_f = std::fabs(f);
            stats.max_abs = std::max(stats.max_abs, abs_f);
            if (abs_f > 1.0e4f) {
                ++stats.huge_count;
            }
        }
        if (!dwords.empty()) {
            stats.head0 = dwords[0];
            stats.head1 = dwords.size() > 1 ? dwords[1] : 0;
        }
        return stats;
    }

    /// Coarse signature so ordinary per-frame animation churn does not spam;
    /// NaN/Inf/huge-count buckets and the max-abs exponent still register.
    static u32 Bucket(const BufferStats& s) {
        const int exp = s.max_abs > 0.0f ? std::min(31, int(std::log2(s.max_abs))) : -1;
        return (std::min<u32>(s.nan_count, 255) << 24) | (std::min<u32>(s.inf_count, 63) << 18) |
               (std::min<u32>(s.huge_count, 1023) << 8) | u32(exp + 1);
    }
};

} // namespace Vulkan
