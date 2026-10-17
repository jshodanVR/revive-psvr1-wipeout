// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cctype>
#include <string_view>

namespace Vulkan {

namespace detail {

[[nodiscard]] inline bool ContainsIgnoreCase(std::string_view haystack, std::string_view needle) {
    if (needle.empty() || haystack.size() < needle.size()) {
        return false;
    }
    for (size_t i = 0; i + needle.size() <= haystack.size(); ++i) {
        bool match = true;
        for (size_t j = 0; j < needle.size(); ++j) {
            const auto a = static_cast<unsigned char>(haystack[i + j]);
            const auto b = static_cast<unsigned char>(needle[j]);
            if (std::tolower(a) != std::tolower(b)) {
                match = false;
                break;
            }
        }
        if (match) {
            return true;
        }
    }
    return false;
}

[[nodiscard]] inline bool ContainsStandaloneToken(std::string_view haystack,
                                                  std::string_view token) {
    for (size_t i = 0; i + token.size() <= haystack.size(); ++i) {
        if (haystack.compare(i, token.size(), token) != 0) {
            continue;
        }
        const bool left_ok =
            i == 0 || !std::isdigit(static_cast<unsigned char>(haystack[i - 1]));
        const bool right_ok =
            i + token.size() == haystack.size() ||
            !std::isdigit(static_cast<unsigned char>(haystack[i + token.size()]));
        if (left_ok && right_ok) {
            return true;
        }
    }
    return false;
}

} // namespace detail

[[nodiscard]] inline bool IsAdreno830Or840(std::string_view device_name) {
    if (!detail::ContainsIgnoreCase(device_name, "adreno")) {
        return false;
    }
    return detail::ContainsStandaloneToken(device_name, "830") ||
           detail::ContainsStandaloneToken(device_name, "840");
}

/// Adreno 830/840 Turnip may mis-convert integer vertex attributes when
/// VK_EXT_legacy_vertex_attributes is enabled. Force shader-side NumberClass.
[[nodiscard]] inline bool ShouldDisableLegacyVertexAttributes(std::string_view device_name) {
    return IsAdreno830Or840(device_name);
}

/// Adreno 830/840 F32 buffer loads may not preserve IEEE bits (denorm/NaN).
/// DSR skinned VS loads bone/extra data as LoadBufferF32x4 #0 then bitcasts.
[[nodiscard]] inline bool ShouldForceBitPreservingBuffer0Loads(std::string_view device_name) {
    return IsAdreno830Or840(device_name);
}

[[nodiscard]] inline bool ShouldLoadFloatBufferAsU32(std::string_view device_name,
                                                    unsigned handle) {
    return handle == 0 && ShouldForceBitPreservingBuffer0Loads(device_name);
}

} // namespace Vulkan
