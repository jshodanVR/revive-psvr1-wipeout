// SPDX-License-Identifier: GPL-2.0-or-later
// CUSA06929 01.00 / VERSION 01.01, UE4 Morpheus settings.
// Override the value passed to r.ScreenPercentage after configuration loading.
// The shipping game's ExecCmds/INI command-line overrides are ignored.
#pragma once
#include <array>
#include <cstring>
#include <span>
#include "common/types.h"
namespace Core::KnownTitle::StatikResolution {
constexpr std::array<size_t, 2> Offsets{0x3256666, 0x325675e};
constexpr std::array<u8, 19> Original{
    0xc4,0xc1,0x7a,0x10,0x86,0xe0,0x05,0x00,0x00,
    0xc5,0xf0,0x57,0xc9,0xc5,0xf8,0x2e,0xc8,0x73,0x44};
constexpr char16_t Cvar[] = u"r.ScreenPercentage";
constexpr size_t HeapOffset = 0x3078df;
constexpr std::array<u8, 22> HeapOriginal{
    0x84,0xc0,0x74,0x12,0x48,0x63,0x45,0xdc,
    0x48,0x8d,0x0d,0xea,0x1e,0x28,0x04,
    0x48,0xc1,0xe0,0x14,0x48,0x89,0x01};
constexpr size_t HeapDefaultsOffset = 0x45897d8;
constexpr std::array<u64, 2> HeapDefaults{2600ULL << 20, 200ULL << 20};
inline constexpr u32 RequiredExtraMemoryMB(u32 percent) {
    return percent <= 100 ? 0 : percent <= 125 ? 512 : percent <= 150 ? 768 : 1024;
}
inline bool Apply(std::span<u8> image, u32 percent, u32 available_extra_mb = 0) {
    if (percent < 100 || percent > 200 || image.size() < 0x38d37c4 + sizeof(Cvar))
        return false;
    if (std::memcmp(image.data() + 0x38d37c4, Cvar, sizeof(Cvar)))
        return false;
    for (size_t i = 0; i < Offsets.size(); ++i) {
        auto expected = Original;
        expected[5] += static_cast<u8>(4 * i);
        if (std::memcmp(image.data() + Offsets[i], expected.data(), expected.size()))
            return false;
    }
    if (percent == 100)
        return true;
    // The PS4 engine partitions direct memory into fixed Garlic (GPU) and
    // Onion pools plus a remainder. Increasing only the emulator's memory
    // enlarges the remainder, leaving render-target allocation failures intact.
    // Validate everything before either eye scale or heap size is changed.
    if (available_extra_mb < RequiredExtraMemoryMB(percent) ||
        image.size() < HeapDefaultsOffset + sizeof(HeapDefaults) ||
        std::memcmp(image.data() + HeapOffset, HeapOriginal.data(), HeapOriginal.size()) ||
        std::memcmp(image.data() + HeapDefaultsOffset, HeapDefaults.data(), sizeof(HeapDefaults)))
        return false;
    // After the INI lookup, force its result through the original 64-bit MB->byte
    // conversion/store. The Onion budget is unchanged. The largest Garlic pool
    // remains below 4 GiB; do not truncate it through a 32-bit byte immediate.
    std::array<u8, 8> heap_patch{0xb8,0,0,0,0,0x90,0x90,0x90};
    const u32 heap_mb = 2600 + RequiredExtraMemoryMB(percent);
    std::memcpy(heap_patch.data() + 1, &heap_mb, sizeof(heap_mb));
    std::memcpy(image.data() + HeapOffset, heap_patch.data(), heap_patch.size());
    for (size_t i = 0; i < Offsets.size(); ++i) {
        // mov eax,<float>; vmovd xmm0,eax. RAX is overwritten by the vtable load
        // before its next use; the following float comparison and Set remain intact.
        std::array<u8, 9> patch{0xb8,0,0,0,0,0xc5,0xf9,0x6e,0xc0};
        const float value = (i == 0 ? 145.f : 180.f) * (percent / 100.f);
        std::memcpy(patch.data() + 1, &value, sizeof(value));
        std::memcpy(image.data() + Offsets[i], patch.data(), patch.size());
    }
    return true;
}
} // namespace Core::KnownTitle::StatikResolution
