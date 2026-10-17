// SPDX-License-Identifier: GPL-2.0-or-later
// CUSA03559 01.06: eye target sizes are 960x1080 times a hardware-profile scale.
// Validate the complete size calculation and both table entries before any write.
#pragma once
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <span>
#include "common/types.h"

namespace Core::KnownTitle::ThumperResolution {
constexpr size_t CodeOffset = 0x1dd718;
constexpr size_t TableOffset = 0x270430;
constexpr std::array<u8, 53> Code{
    0x48,0x8d,0x05,0x11,0x2d,0x09,0x00,0x4c,0x89,0xa4,0x24,0x98,0x00,0x00,0x00,
    0x0f,0x94,0xc1,0x0f,0xb6,0xd9,0xc5,0xfa,0x10,0x04,0x98,0x83,0xcb,0x02,
    0xc5,0xfa,0x59,0x0d,0xdb,0xdb,0x08,0x00,0xc5,0xfa,0x59,0x05,0xd7,0xdb,0x08,0x00,
    0xc5,0x7a,0x2c,0xe9,0xc5,0xfa,0x2c,0xc0};
constexpr std::array<float, 2> Original{1.4f, 1.6f};
inline bool Apply(std::span<u8> image, u32 percent) {
    if (percent < 100 || percent > 150 || image.size() < TableOffset + sizeof(Original))
        return false;
    constexpr std::array<float, 2> Base{960.f, 1080.f};
    if (std::memcmp(image.data() + CodeOffset, Code.data(), Code.size()) ||
        std::memcmp(image.data() + 0x26b318, Base.data(), sizeof(Base)) ||
        std::memcmp(image.data() + TableOffset, Original.data(), sizeof(Original)))
        return false;
    if (percent == 100)
        return true;
    auto scaled = Original;
    for (auto& value : scaled) {
        // Guest uses truncation. Avoid losing a pixel to float rounding at e.g. 150%.
        value = std::nextafter(value * (percent / 100.f), std::numeric_limits<float>::infinity());
    }
    std::memcpy(image.data() + TableOffset, scaled.data(), sizeof(scaled));
    return true;
}
} // namespace Core::KnownTitle::ThumperResolution
