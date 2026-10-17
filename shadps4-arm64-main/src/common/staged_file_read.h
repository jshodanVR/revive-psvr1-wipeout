// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <algorithm>
#include <cstddef>
#include <cstring>
#include <vector>
namespace Common::FS {
// OS reads cannot fault through the emulator's page-tracking handler. Stage in
// host memory, then use CPU stores so protected guest pages can be untracked.
// Keep storage bounded and reusable even for very large guest requests.
template <typename Reader>
std::size_t ReadStaged(void* destination, std::size_t size, Reader&& read) {
    constexpr std::size_t ChunkSize = 1024 * 1024;
    thread_local std::vector<std::byte> staging;
    if (size == 0) {
        return 0;
    }
    staging.resize(std::min(size, ChunkSize));
    std::size_t total = 0;
    while (total < size) {
        const auto requested = std::min(size - total, staging.size());
        const auto actual = read(staging.data(), requested);
        if (actual != 0) {
            std::memcpy(static_cast<std::byte*>(destination) + total, staging.data(), actual);
        }
        total += actual;
        if (actual < requested) {
            break;
        }
    }
    return total;
}
} // namespace Common::FS
