// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <algorithm>
#include <cstdint>

namespace VideoCore {
struct StreamGrowth {
    bool left;
    bool right;
    uint64_t bytes;
};

// Reserve at the edge the new request extends, not at the opposite edge
// contributed by the old buffer. Keep speculative capacity bounded.
constexpr StreamGrowth RequestedStreamGrowth(uint64_t begin, uint64_t end,
                                            uint64_t old_begin, uint64_t old_end) {
    constexpr uint64_t MiB = 1024 * 1024;
    return {begin < old_begin, end > old_end,
            std::clamp((old_end - old_begin) / 4, 2 * MiB, 64 * MiB)};
}
} // namespace VideoCore
