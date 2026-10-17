// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <algorithm>
#include <cstdint>
#include <span>
#include <vector>

namespace AmdGpu::HtileStencil {

enum class Layout { P8_32x32_16x16, RushNeoP16 };

struct MaskRect {
    uint32_t x, y, width, height;
};

// Pixel-center mapping for nearest coverage when a source mask is resized.
constexpr uint32_t MapMaskEdge(uint32_t edge, uint32_t source, uint32_t target) {
    if (!source || !target) return 0;
    return static_cast<uint32_t>((uint64_t{2} * edge * target + source - 1) /
                                 (uint64_t{2} * source));
}

// Linear sampling reaches half a source pixel beyond nearest coverage. Exclude
// that footprint too, otherwise undefined color outside the mask bleeds in.
constexpr uint32_t MapFilteredMaskEdge(uint32_t edge, uint32_t source, uint32_t target,
                                      bool upper, uint32_t radius_twice = 1) {
    if (!source || !target) return 0;
    const int64_t n = (int64_t{2} * edge + (upper ? int64_t{radius_twice} : -int64_t{radius_twice})) *
                      target + source - 1;
    return static_cast<uint32_t>(std::clamp(n / (int64_t{2} * source), int64_t{0}, int64_t{target}));
}

inline std::vector<MaskRect> HiddenRects(std::span<const uint8_t> stencil,
                                       uint32_t width, uint32_t height) {
    std::vector<MaskRect> rects;
    if (!width || !height || stencil.size() != uint64_t{width} * height) return rects;
    // DecodeRushMask guarantees constant coverage within each 8x8 tile.
    for (uint32_t y = 0; y < height; y += 8) {
        uint32_t x = 0;
        while (x < width) {
            if (stencil[size_t{y} * width + x] != 0) {
                x += 8;
                continue;
            }
            const auto start = x;
            do {
                x += 8;
            } while (x < width && stencil[size_t{y} * width + x] == 0);
            rects.push_back({start, y, std::min(x, width) - start, std::min(8u, height - y)});
        }
    }
    return rects;
}

// SI/CI P8 HTILE and Rush's CPU-written Neo template, 256-byte interleave.
// Address equations follow AMD addrlib (MIT), SiLib::HwlComputeXmaskAddrFromCoord
// and TileCoordToMaskElementIndex. See LICENSES/AMD-addrlib-MIT.txt.
constexpr uint32_t WordIndex(uint32_t x, uint32_t y, uint32_t aligned_pitch,
                             Layout layout = Layout::P8_32x32_16x16) {
    const uint32_t tx = x / 8;
    const uint32_t ty = y / 8;
    if (layout == Layout::RushNeoP16) {
        // Rush's CPU writer (RVA 0xcc2700) retains the P8 pipe bit 2 equation
        // and adds bit 3. This differs from generic addrlib P16_32x32_16x16.
        // Equal pass/fail counts cannot detect that permutation difference.
        const uint32_t pipe = ((tx ^ ty ^ (tx >> 1)) & 1) |
                              ((((tx >> 1) ^ (ty >> 1)) & 1) << 1) |
                              ((((tx >> 2) ^ (ty >> 2)) & 1) << 2) |
                              ((((tx >> 3) ^ (ty >> 2)) & 1) << 3);
        const uint32_t element = (((tx >> 1) ^ ty) & 1) | (tx & 2);
        const uint32_t micro = (((x % 1024) / 32 + (y % 512) / 32 * 32) & ~3u) | element;
        const uint32_t offset =
            ((x / 1024 + y / 512 * (aligned_pitch / 1024)) * 512 + micro) * 4;
        return ((offset & 255) | (pipe << 8) | ((offset >> 8) << 12)) / 4;
    }
    const uint32_t pipe = ((tx ^ ty ^ (tx >> 1)) & 1) |
                          ((((tx >> 1) ^ (ty >> 1)) & 1) << 1) |
                          ((((tx >> 2) ^ (ty >> 2)) & 1) << 2);
    const uint32_t element = (((tx >> 1) ^ ty) & 1) | (tx & 2);
    const uint32_t micro = ((((x % 512) / 32 + (y % 512) / 32 * 16) * 2) & ~3u) |
                           element;
    const uint32_t offset = ((x / 512 + y / 512 * (aligned_pitch / 512)) * 512 + micro) * 4;
    return ((offset & 255) | (pipe << 8) | ((offset >> 8) << 11)) / 4;
}

constexpr uint32_t WordCount(uint32_t pitch, uint32_t height,
                             Layout layout = Layout::P8_32x32_16x16) {
    if (layout == Layout::RushNeoP16) {
        return ((pitch + 1023) / 1024) * ((height + 511) / 512) * 8192;
    }
    return ((pitch + 511) / 512) * ((height + 511) / 512) * 4096;
}

// Expand the immutable visibility template observed in Rush: SR0=1 is all fail,
// SR0=2 is all pass for reference 16, mask 255, LessEqual. These are equivalent
// test values, NOT a reconstruction of the original per-pixel stencil bytes.
// Do not use for arbitrary HTILE, stencil writes or other comparison states.
// Check every visible tile before changing output; unknown encodings fail closed.
inline bool DecodeRushMask(std::span<const uint32_t> words, uint32_t width, uint32_t height,
                           uint32_t pitch, std::vector<uint8_t>& output,
                           Layout layout = Layout::P8_32x32_16x16) {
    // The 200% Neo target is side-by-side 5376x3024; pitch may round to 6144.
    // Bound the output while allowing both eyes at that verified target size.
    if (!width || !height || width > 8192 || height > 4096 || pitch < width || pitch > 8192 ||
        words.size() < WordCount(pitch, height, layout)) {
        return false;
    }
    const uint32_t alignment = layout == Layout::RushNeoP16 ? 1024 : 512;
    const uint32_t aligned_pitch = (pitch + alignment - 1) & ~(alignment - 1);
    for (uint32_t y = 0; y < height; y += 8) {
        for (uint32_t x = 0; x < width; x += 8) {
            const auto word = words[WordIndex(x, y, aligned_pitch, layout)];
            if (word != 0x10 && word != 0x20) {
                return false;
            }
        }
    }
    output.resize(size_t{width} * height);
    for (uint32_t y = 0; y < height; y += 8) {
        for (uint32_t x = 0; x < width; x += 8) {
            const uint8_t value = words[WordIndex(x, y, aligned_pitch, layout)] == 0x20 ? 16 : 0;
            for (uint32_t row = y; row < std::min(y + 8, height); ++row) {
                std::fill_n(output.data() + size_t{row} * width + x, std::min(8u, width - x), value);
            }
        }
    }
    return true;
}
} // namespace AmdGpu::HtileStencil
