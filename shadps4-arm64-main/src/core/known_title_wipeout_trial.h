// SPDX-License-Identifier: GPL-2.0-or-later
// Experimental CUSA05670/01.07 command handoff. Disabled unless explicitly enabled.
// The worker must own the command mutex before waking its producer; otherwise
// the producer can signal before the worker enters its unconditional cond_wait.
#pragma once
#include <algorithm>
#include <array>
#include <cstring>
#include <span>
#include "common/types.h"
namespace Core::KnownTitle::WipeoutTrial {
constexpr u32 StartupEntry = 0x7fdc6f, StartupCave = 0x966a00;
constexpr std::array<u8, 11> StartupExpected{
    0x48, 0x89, 0xdf, 0x4c, 0x89, 0xfe, 0xe8, 0x16, 0x1a, 0x00, 0x00};
// RBX = tracker + 0x60. The caller already owns the command mutex.
// A signal issued before the worker's FIRST cond_wait is not latched. Check
// both command bytes under that mutex and consume an already pending command.
// Otherwise retain the real cond_wait and its original error handling.
constexpr std::array<u8, 30> StartupCode{
    0x66, 0x83, 0x7b, 0xb8, 0x00, // cmp word [rbx-0x48], 0
    0x75, 0x10,                   // jne pending
    0x48, 0x89, 0xdf,             // mov rdi, rbx
    0x4c, 0x89, 0xfe,             // mov rsi, r15
    0xe8, 0, 0, 0, 0,            // call scePthreadCondWait
    0xe9, 0, 0, 0, 0,            // jmp original return site
    0x31, 0xc0,                   // pending: xor eax, eax
    0xe9, 0, 0, 0, 0};           // jmp original return site
constexpr std::array<u8, 83> Expected{
    0x48, 0x8b, 0xb8, 0xc8, 0x0a, 0x00, 0x00, 0xbe, 0x01, 0x00, 0x00, 0x00, 0xe8, 0x38,
    0x30, 0x00, 0x00, 0x4c, 0x8b, 0xbd, 0xb8, 0xfd, 0xff, 0xff, 0x48, 0x8b, 0x9d, 0xb0,
    0xfd, 0xff, 0xff, 0x89, 0xc1, 0x85, 0xc9, 0x79, 0x10, 0x31, 0xc0, 0x48, 0x8d, 0x3d,
    0xf4, 0x4c, 0x0a, 0x00, 0x89, 0xce, 0xe8, 0xf4, 0x0d, 0x00, 0x00, 0x48, 0x89, 0xdf,
    0xe8, 0xac, 0x11, 0x00, 0x00, 0x89, 0xc1, 0x85, 0xc9, 0x79, 0x10, 0x31, 0xc0, 0x48,
    0x8d, 0x3d, 0xf8, 0x4b, 0x0a, 0x00, 0x89, 0xce, 0xe8, 0xd6, 0x0d, 0x00, 0x00};
constexpr std::array<u8, 83> Replacement{
    0x4c, 0x8b, 0xbd, 0xb8, 0xfd, 0xff, 0xff, 0x48, 0x8b, 0x9d, 0xb0, 0xfd, 0xff, 0xff,
    0x48, 0x89, 0xdf, 0xe8, 0xd3, 0x11, 0x00, 0x00, 0x89, 0xc1, 0x85, 0xc9, 0x79, 0x10,
    0x31, 0xc0, 0x48, 0x8d, 0x3d, 0x1f, 0x4c, 0x0a, 0x00, 0x89, 0xce, 0xe8, 0xfd, 0x0d,
    0x00, 0x00, 0x48, 0x8b, 0xbb, 0x60, 0x0a, 0x00, 0x00, 0xbe, 0x01, 0x00, 0x00, 0x00,
    0xe8, 0x0c, 0x30, 0x00, 0x00, 0x89, 0xc1, 0x85, 0xc9, 0x79, 0x10, 0x31, 0xc0, 0x48,
    0x8d, 0x3d, 0xd6, 0x4c, 0x0a, 0x00, 0x89, 0xce, 0xe8, 0xd6, 0x0d, 0x00, 0x00};

inline u32 Hash(std::span<const u8> bytes) {
    u32 hash = 2166136261u;
    for (const u8 byte : bytes)
        hash = (hash ^ byte) * 16777619u;
    return hash;
}
inline bool Apply(std::span<u8> image) {
    if (image.size() < StartupCave + StartupCode.size())
        return false;
    if (Hash(image.subspan(0x7fdc20, 0x254)) != 0x8d7c45d6u)
        return false;
    if (Hash(image.subspan(0x658e0e, 0xa4)) != 0x5816a488u)
        return false;
    if (std::memcmp(image.data() + 0x7fddc7, Expected.data(), Expected.size()))
        return false;
    if (std::memcmp(image.data() + StartupEntry, StartupExpected.data(), StartupExpected.size()) ||
        !std::all_of(image.begin() + StartupCave, image.begin() + StartupCave + StartupCode.size(),
                     [](u8 byte) { return byte == 0; }))
        return false;
    // All guards precede all writes: unknown layouts and occupied padding are unchanged.
    std::memcpy(image.data() + StartupCave, StartupCode.data(), StartupCode.size());
    const auto relative = [&](u32 operand, u32 next, u32 target) {
        const s32 displacement = static_cast<s32>(target) - static_cast<s32>(next);
        std::memcpy(image.data() + operand, &displacement, sizeof(displacement));
    };
    relative(StartupCave + 14, StartupCave + 18, 0x7ff690);
    relative(StartupCave + 19, StartupCave + 23, 0x7fdc7a);
    relative(StartupCave + 26, StartupCave + 30, 0x7fdc7a);
    std::fill_n(image.data() + StartupEntry, StartupExpected.size(), 0x90);
    image[StartupEntry] = 0xe9;
    relative(StartupEntry + 1, StartupEntry + 5, StartupCave);
    std::memcpy(image.data() + 0x7fddc7, Replacement.data(), Replacement.size());
    return true;
}
} // namespace Core::KnownTitle::WipeoutTrial
