// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <algorithm>
#include <array>
#include <cstring>
#include <span>
#include <vector>
#include "common/sha256.h"
#include "common/types.h"

namespace Core::KnownTitle::FarpointStack {

// CUSA04508 01.08 texture tiling leaf function. Windows fault delivery can
// overwrite the SysV red zone used for its stride temporaries. Preserve the
// function's computation while moving all 14 stack operands into allocated
// space. Its only branch is an internal loop; there are no RIP-relative operands.
// No copyrighted function body is embedded: copy the verified loaded image.
inline bool Apply(std::span<u8> image) {
    constexpr size_t entry = 0x2b52810, length = 0x4df, epilogue = 0x4d4;
    constexpr size_t cave = 0x4547500;
    constexpr std::array<size_t, 14> displacements{
        0x46, 0x58, 0x5d, 0x62, 0x82, 0x8c, 0x9c,
        0xa8, 0xad, 0xb2, 0x1f9, 0x21f, 0x277, 0x4b5};
    constexpr std::array<u8, 5> reserve{0x48, 0x8d, 0x64, 0x24, 0xc0};
    constexpr std::array<u8, 5> release{0x48, 0x8d, 0x64, 0x24, 0x40};
    if (image.size() < cave + length + 10 || image.size() < entry + length) {
        return false;
    }
    const auto original = image.subspan(entry, length);
    if (sha256::SHA256::Hex(std::string_view{
            reinterpret_cast<const char*>(original.data()), original.size()}) !=
            "4fc45b1baf7c3d3d8aca768f845ddb95658bb7faf4c9b00f8d7051c5306ea674" ||
        !std::all_of(image.begin() + cave, image.begin() + cave + length + 10,
                     [](u8 value) { return value == 0; })) {
        return false;
    }
    std::vector<u8> patched(original.begin(), original.end());
    for (const auto at : displacements) {
        patched[at] = static_cast<u8>(patched[at] + 64);
    }
    // Insert from the end so the original offsets stay valid. LEA preserves flags.
    patched.insert(patched.begin() + epilogue, release.begin(), release.end());
    patched.insert(patched.begin() + 10, reserve.begin(), reserve.end());
    std::memcpy(image.data() + cave, patched.data(), patched.size());
    const s32 jump = static_cast<s32>(cave - entry - 5);
    image[entry] = 0xe9;
    std::memcpy(image.data() + entry + 1, &jump, sizeof(jump));
    return true;
}

} // namespace Core::KnownTitle::FarpointStack
