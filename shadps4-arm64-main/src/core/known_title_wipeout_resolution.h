// SPDX-License-Identifier: GPL-2.0-or-later
// CUSA05670 01.07: PSVR eye allocation scale (base/Neo), before dynamic resolution.
#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <span>
#include "core/known_title_wipeout_trial.h"
namespace Core::KnownTitle::WipeoutResolution {
inline constexpr u32 RequiredExtraMemoryMB(u32 percent) {
    return percent <= 100 ? 0 : percent <= 125 ? 512 : percent <= 150 ? 1024 : 2048;
}
// The game reserves its direct-memory heap from these base/Neo configuration
// tables; increasing the emulator's available memory alone leaves it unchanged.
inline bool ApplyMemory(std::span<u8> image, u32 extra_mb) {
    constexpr std::array<size_t, 3> offsets{0x86bb60, 0x86bb80, 0x86e4e0};
    constexpr std::array<u64, 4> base{0x108000000, 0x10000000, 0x8000, 0x57000};
    if (image.size() < offsets.back() + sizeof(base) || extra_mb > 2048 ||
        extra_mb % 256 != 0)
        return false;
    for (size_t i = 0; i < offsets.size(); ++i) {
        auto expected = base;
        if (i == 1) expected[0] = 0x128000000;
        if (std::memcmp(image.data() + offsets[i], expected.data(), sizeof(expected)))
            return false;
    }
    for (size_t i = 0; i < offsets.size(); ++i) {
        const u64 size = (i == 1 ? 0x128000000ULL : base[0]) + (u64(extra_mb) << 20);
        std::memcpy(image.data() + offsets[i], &size, sizeof(size));
    }
    return true;
}
// Keep all 16 resolution-level resources allocated: disabling their constructor
// flag leaves null targets in the race. Both floats in each table entry must agree.
inline constexpr size_t DynamicTableOffset = 0x86bba0;
inline bool ApplyFixed(std::span<u8> image, bool neo = false) {
    if (image.size() < 0x86ed08 ||
        WipeoutTrial::Hash(image.subspan(DynamicTableOffset, 128)) != 0x8bdb42a1u ||
        WipeoutTrial::Hash(image.subspan(0x7fa681, 0x130)) != 0x85279202u)
        return false;
    float scale{};
    std::memcpy(&scale, image.data() + 0x86ed00 + (neo ? 4 : 0), sizeof(scale));
    if (!std::isfinite(scale) || scale < 1.4f || scale > 3.001f)
        return false;
    // Level views round to the nearest multiple of 8, unlike the main allocation.
    // Keep their dimensions within that allocation (e.g. 2268 must not become 2272).
    const u32 width = static_cast<u32>(960.f * scale + .5f);
    const u32 height = static_cast<u32>(1080.f * scale + .5f);
    const float full = std::min(static_cast<float>(width & ~7u) / width,
                                static_cast<float>(height & ~7u) / height);
    for (size_t i = 0; i < 32; ++i)
        std::memcpy(image.data() + DynamicTableOffset + i * sizeof(float), &full, sizeof(full));
    return true;
}
inline bool Apply(std::span<u8> image, u32 percent, u32 available_extra_mb = 0) {
    constexpr std::array<float, 2> original{1.4f, 1.5f};
    if (percent < 100 || percent > 200 ||
        available_extra_mb < RequiredExtraMemoryMB(percent) || image.size() < 0x86ed08 ||
        WipeoutTrial::Hash(image.subspan(0x7f80e7, 32)) != 0x608ee9b8u ||
        WipeoutTrial::Hash(image.subspan(0x7fa681, 0x130)) != 0x85279202u ||
        std::memcmp(image.data() + 0x86ed00, original.data(), sizeof(original)))
        return false;
    if (percent == 100)
        return true;
    // All scale/layout checks precede heap writes. An unknown heap table rejects
    // the entire request, so a larger eye allocation cannot exhaust the old heap.
    if (!ApplyMemory(image, RequiredExtraMemoryMB(percent)))
        return false;
    auto scaled = original;
    for (auto& value : scaled)
        value = std::nextafter(value * (percent / 100.f), std::numeric_limits<float>::infinity());
    std::memcpy(image.data() + 0x86ed00, scaled.data(), sizeof(scaled));
    return true;
}
} // namespace Core::KnownTitle::WipeoutResolution
