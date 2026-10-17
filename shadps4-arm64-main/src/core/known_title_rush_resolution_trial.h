// SPDX-License-Identifier: GPL-2.0-or-later
// Experimental: CUSA02350 01.00. Scale the two startup render-size entries,
// leaving output-panel dimensions unchanged. Grow the backing render pool too.
#pragma once
#include <array>
#include <atomic>
#include <cstring>
#include <span>
#include "common/types.h"

namespace Core::KnownTitle::RushResolution {
inline std::atomic<u32> active_percent{100};
inline std::atomic<bool> active_alignment{false};
constexpr u32 RequiredExtraMemoryMB(u32 percent) {
    return percent == 125 ? 512 : percent == 150 ? 1280 : percent == 200 ? 2560 : 0;
}
constexpr u32 Scaled(u32 value, u32 percent, bool align_tiles) {
    const u32 pixels = value * percent / 100;
    return align_tiles && percent != 100 ? (pixels + 7) / 8 * 8 : pixels;
}
constexpr size_t PoolOffset = 0x1999a4;
constexpr std::array<u8, 23> PoolOriginal{
    0xba,0x00,0x00,0xf0,0x32,0xb9,0x00,0x00,0x70,0x26,
    0x85,0xc0,0x48,0x0f,0x4f,0xca,0x48,0x89,0x8b,0xb0,0x00,0x00,0x00};
constexpr size_t TableOffset = 0x1a090e0;
constexpr size_t SelectionOffset = 0x7a3859;
constexpr std::array<u8, 20> Selection{
    0x0f,0x95,0xc0,0x0f,0xb6,0xc0,0x48,0xc1,0xe0,0x05,
    0x48,0x8b,0x54,0x08,0x20,0x48,0x8b,0x44,0x08,0x28};
constexpr auto Original = [] {
    std::array<u32, 40> table{};
    for (u32 i = 0; i < 10; ++i) {
        table[i * 4 + 2] = 1920 + 192 * i;
        table[i * 4 + 3] = 1080 + 108 * i;
    }
    return table;
}();
inline bool Apply(std::span<u8> image, u32 percent, bool align_tiles, u32 extra_memory_mb,
                  u32 memory_percent = 0) {
    if (!memory_percent) memory_percent = percent;
    active_percent = 100;
    active_alignment = false;
    if ((percent != 100 && percent != 125 && percent != 150 && percent != 200) ||
        (memory_percent != 100 && memory_percent != 125 && memory_percent != 150 && memory_percent != 200) ||
        memory_percent < percent ||
        extra_memory_mb < RequiredExtraMemoryMB(memory_percent) ||
        image.size() < TableOffset + sizeof(Original) ||
        std::memcmp(image.data() + PoolOffset, PoolOriginal.data(), PoolOriginal.size()) ||
        std::memcmp(image.data() + TableOffset, Original.data(), sizeof(Original)) ||
        std::memcmp(image.data() + SelectionOffset, Selection.data(), Selection.size())) {
        return false;
    }
    // The selected entries are 2 for base hardware and 4 for the Neo path.
    // Check the entire table and selector before making either write.
    const auto scaled = [&](u32 value) {
        return Scaled(value, percent, align_tiles);
    };
    for (u32 entry : {2u, 4u}) {
        const std::array<u32, 2> size{
            scaled(Original[entry * 4 + 2]), scaled(Original[entry * 4 + 3])};
        std::memcpy(image.data() + TableOffset + entry * 16 + 8, size.data(), sizeof(size));
    }
    // The same region size drives direct allocation/mapping, remaining-heap
    // accounting and allocator initialization. Never enlarge only its free list.
    const u32 growth = RequiredExtraMemoryMB(memory_percent) * 1024 * 1024;
    const u32 neo_pool = 0x32f00000 + growth;
    const u32 base_pool = 0x26700000 + growth;
    std::memcpy(image.data() + PoolOffset + 1, &neo_pool, sizeof(neo_pool));
    std::memcpy(image.data() + PoolOffset + 6, &base_pool, sizeof(base_pool));
    active_alignment = align_tiles;
    active_percent = percent;
    return true;
}
} // namespace Core::KnownTitle::RushResolution
