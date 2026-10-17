// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#ifdef _WIN32
#include <windows.h>
#include <algorithm>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <vector>

namespace Common {

struct VirtualProtectionSpan {
    uintptr_t address;
    SIZE_T size;
    DWORD protection;
};

// A mapped view can contain many page protections even though its allocation
// was created with one default. Capture them before replacing its placeholder.
inline std::optional<std::vector<VirtualProtectionSpan>> CaptureVirtualProtections(
    HANDLE process, uintptr_t address, SIZE_T size) {
    if (!size || address > std::numeric_limits<uintptr_t>::max() - size) return std::nullopt;
    const auto end = address + size;
    std::vector<VirtualProtectionSpan> result;
    for (auto cursor = address; cursor < end;) {
        MEMORY_BASIC_INFORMATION info{};
        if (!VirtualQueryEx(process, reinterpret_cast<void*>(cursor), &info, sizeof(info)) ||
            info.State != MEM_COMMIT) return std::nullopt;
        const auto base = reinterpret_cast<uintptr_t>(info.BaseAddress);
        if (!info.RegionSize || base > cursor ||
            base > std::numeric_limits<uintptr_t>::max() - info.RegionSize ||
            base + info.RegionSize <= cursor) return std::nullopt;
        const auto next = (std::min)(end, base + info.RegionSize);
        result.push_back({cursor, next - cursor, info.Protect});
        cursor = next;
    }
    return result;
}

// Call separately for each replacement view: VirtualProtectEx cannot span
// independently allocated views, even when their addresses are contiguous.
inline bool RestoreVirtualProtections(HANDLE process, uintptr_t address, SIZE_T size,
                                     std::span<const VirtualProtectionSpan> spans) {
    if (!size || address > std::numeric_limits<uintptr_t>::max() - size) return false;
    const auto end = address + size;
    auto cursor = address;
    for (const auto& span : spans) {
        if (!span.size || span.address > std::numeric_limits<uintptr_t>::max() - span.size)
            return false;
        const auto begin = (std::max)(address, span.address);
        const auto finish = (std::min)(end, span.address + span.size);
        if (begin >= finish) continue;
        if (begin != cursor) return false;
        DWORD previous{};
        if (!VirtualProtectEx(process, reinterpret_cast<void*>(begin), finish - begin,
                              span.protection, &previous)) return false;
        cursor = finish;
    }
    return cursor == end;
}

} // namespace Common
#endif
