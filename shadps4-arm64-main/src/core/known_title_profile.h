// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <bit>
#include <cstring>
#include <span>
#include <string_view>
#include <vector>

#include "common/types.h"

namespace Core::KnownTitle::Profiles {

inline constexpr std::array<std::array<u32, 2>, 7> ConsoleSizes{
    {{640, 360}, {1280, 720}, {1920, 1080}, {816, 870}, {960, 1080}, {1200, 1280}, {1440, 1536}}};
inline constexpr u32 ConsoleTargetPool = 0xc800000;
inline constexpr u32 ConsoleSmallPool = 0xa00000;
inline constexpr u64 ConsoleGraphicsHeap = 0x36800000;
inline constexpr std::array<u8, 9> RecentreCode{0x40, 0x0f, 0xb6, 0xc6, 0xff,
                                                0xc0, 0x89, 0x07, 0xc3};

struct Profile {
    std::string_view name;
    u64 recentre;
    u64 manager_pointer;
    u64 frame_rate;
    u64 frame_seconds;
    u64 frame_microseconds;
    u64 resolution_pointer;
    u64 widths;
    u64 heights;
    std::array<std::array<u64, 2>, 4> size_switch; // height, width immediates
    u64 pixels;                                    // u64 pixel counts, 32-byte stride
    std::array<u64, 4> eyes;                       // width, height, width, height immediates
    std::array<u64, 2> target_pool;
    std::array<u64, 3> small_pool;
    u64 graphics_heap;
};

struct CodeCheck {
    u64 at;
    std::array<u8, 11> bytes;
    u32 size;
};

// Cross-check the alternate layout's singleton references and the engine's
// time-step getter, not just data constants that happen to have familiar values.
inline constexpr std::array<CodeCheck, 3> AlternateCode{{
    {0x40d7, {0x48, 0x8d, 0x1d, 0x82, 0xbd, 0xec, 0x02}, 7},
    {0xcc613b, {0x4c, 0x8d, 0x3d, 0xa6, 0xc9, 0x20, 0x02}, 7},
    {0xecc560, {0x48, 0x8d, 0x05, 0x61, 0x43, 0x85, 0x00, 0x48, 0x8b, 0x00, 0xc3}, 11},
}};

// The first layout is preserved from upstream. The second was located independently
// in a CUSA12392 dump reporting APP_VER 01.00, VERSION 01.04 and 01.04 change notes
// (eboot SHA256 95ed2d85c176398cabab0ef2b565c2713fd536dcdb7cb8ba0b8c5bedc5831500).
// Neither metadata nor change notes prove an executable's version: identify its layout.
inline constexpr std::array<Profile, 2> Known{{
    {"CUSA12392/c48320 (upstream)",
     0xc48320,
     0x2e025a8,
     0x16688a8,
     0x16688b0,
     0x16688b8,
     0x2dff9e0,
     0x12da900,
     0x12da920,
     {{{0xf2242f, 0xf22435}, {0xf2240d, 0xf22413}, {0xf22551, 0xf22557}, {0xf2255f, 0xf22565}}},
     0x1645048,
     {0xc3fad0, 0xc3fad5, 0xc3fb5c, 0xc3fb61},
     {0xef8bf7, 0xef8c4d},
     {0xf22194, 0xf221be, 0xf221dd},
     0x1269708},
    {"CUSA12392/ccc0b0",
     0xccc0b0,
     0x2ed2ae8,
     0x17208b8,
     0x17208c0,
     0x17208c8,
     0x2ecfe60,
     0x1371120,
     0x1371140,
     {{{0xfa5a1f, 0xfa5a25}, {0xfa59fd, 0xfa5a03}, {0xfa5b41, 0xfa5b47}, {0xfa5b4f, 0xfa5b55}}},
     0x16fb558,
     {0xcc3580, 0xcc3585, 0xcc360c, 0xcc3611},
     {0xf7c117, 0xf7c16d},
     {0xfa5784, 0xfa57ae, 0xfa57cd},
     0x12ffb78},
}};

struct Change {
    u64 at;
    u64 was;
    u64 now;
    u32 bytes;
};

inline bool Contains(std::span<const u8> image, u64 at, u64 bytes) {
    return at <= image.size() && bytes <= image.size() - at;
}

inline bool Equals(std::span<const u8> image, u64 at, u64 value, u32 bytes) {
    return bytes <= sizeof(value) && Contains(image, at, bytes) &&
           std::memcmp(image.data() + at, &value, bytes) == 0;
}

inline std::vector<Change> ResolutionChanges(const Profile& profile,
                                             const std::array<std::array<u32, 2>, 7>& sizes,
                                             u32 target_pool, u32 small_pool, u64 heap) {
    std::vector<Change> changes;
    for (u32 level = 3; level <= 6; ++level) {
        const auto& was = ConsoleSizes[level];
        const auto& now = sizes[level];
        changes.push_back({profile.widths + 4 * level, was[0], now[0], 4});
        changes.push_back({profile.heights + 4 * level, was[1], now[1], 4});
        changes.push_back({profile.size_switch[level - 3][0], was[1], now[1], 4});
        changes.push_back({profile.size_switch[level - 3][1], was[0], now[0], 4});
        changes.push_back(
            {profile.pixels + 32 * level, u64{was[0]} * was[1], u64{now[0]} * now[1], 8});
    }
    for (u32 i = 0; i < profile.eyes.size(); ++i) {
        changes.push_back({profile.eyes[i], ConsoleSizes[6][i % 2], sizes[6][i % 2], 4});
    }
    for (const u64 at : profile.target_pool) {
        changes.push_back({at, ConsoleTargetPool, target_pool, 4});
    }
    for (const u64 at : profile.small_pool) {
        changes.push_back({at, ConsoleSmallPool, small_pool, 4});
    }
    changes.push_back({profile.graphics_heap, ConsoleGraphicsHeap, heap, 8});
    return changes;
}

// Check every destination before the first write: even a mismatch at the last
// address must leave the image completely untouched.
inline bool Apply(std::span<u8> image, std::span<const Change> changes, u64& rejected_at) {
    for (const auto& change : changes) {
        if (!Equals(image, change.at, change.was, change.bytes)) {
            rejected_at = change.at;
            return false;
        }
    }
    for (const auto& change : changes) {
        std::memcpy(image.data() + change.at, &change.now, change.bytes);
    }
    return true;
}

// Called once on the freshly mapped title, before it executes or is patched.
// A nine-byte code signature alone is insufficient: also validate every patch
// location, all seven resolution levels and the initial engine time fields.
inline bool Matches(std::span<const u8> image, const Profile& profile) {
    if (!Contains(image, profile.recentre, RecentreCode.size()) ||
        std::memcmp(image.data() + profile.recentre, RecentreCode.data(), RecentreCode.size()) ||
        !Contains(image, profile.manager_pointer, sizeof(u64)) ||
        !Contains(image, profile.resolution_pointer, sizeof(u64)) ||
        !Equals(image, profile.frame_rate, std::bit_cast<u64>(60.0), 8) ||
        !Equals(image, profile.frame_seconds, std::bit_cast<u32>(1.0f / 60.0f), 4) ||
        !Equals(image, profile.frame_microseconds, 16666, 8)) {
        return false;
    }
    if (profile.recentre == Known[1].recentre) {
        for (const auto& check : AlternateCode) {
            if (!Contains(image, check.at, check.size) ||
                std::memcmp(image.data() + check.at, check.bytes.data(), check.size)) {
                return false;
            }
        }
    }
    for (u32 i = 0; i < ConsoleSizes.size(); ++i) {
        if (!Equals(image, profile.widths + 4 * i, ConsoleSizes[i][0], 4) ||
            !Equals(image, profile.heights + 4 * i, ConsoleSizes[i][1], 4) ||
            !Equals(image, profile.pixels + 32 * i, u64{ConsoleSizes[i][0]} * ConsoleSizes[i][1],
                    8)) {
            return false;
        }
    }
    for (const auto& check : ResolutionChanges(profile, ConsoleSizes, ConsoleTargetPool,
                                               ConsoleSmallPool, ConsoleGraphicsHeap)) {
        if (!Equals(image, check.at, check.was, check.bytes)) {
            return false;
        }
    }
    return true;
}

inline const Profile* Detect(std::span<const u8> image, std::string_view serial) {
    if (serial != "CUSA12392") {
        return nullptr;
    }
    const Profile* match = nullptr;
    for (const auto& profile : Known) {
        if (Matches(image, profile)) {
            if (match != nullptr) {
                return nullptr; // ambiguous: never guess
            }
            match = &profile;
        }
    }
    return match;
}

} // namespace Core::KnownTitle::Profiles
