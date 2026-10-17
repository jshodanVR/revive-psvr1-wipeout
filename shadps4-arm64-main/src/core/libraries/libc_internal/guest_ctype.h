// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <array>
#include <cstdint>
namespace Libraries::LibcInternal {
// Guest Dinkumware C-locale masks. These differ from the host Windows CRT masks.
// In particular Farpoint's NP toolkit tests 0x02 for uppercase hexadecimal letters.
struct GuestCtypeTables {
    std::array<std::uint16_t, 384> classes{};
    std::array<std::int16_t, 384> lower{};
    std::array<std::int16_t, 384> upper{};
    constexpr const std::uint16_t* data() const { return classes.data(); }
    constexpr GuestCtypeTables() {
        for (int c = -128; c < 256; ++c) {
            lower[c + 128] = c;
            upper[c + 128] = c;
            if (c < 0 || c >= 128)
                continue;
            std::uint16_t flags = 0;
            if (c < 32 || c == 127)
                flags = c >= 9 && c <= 13 ? 0x40 : 0x80;
            else if (c == ' ')
                flags = 0x04;
            else if (c >= '0' && c <= '9')
                flags = 0x21;
            else if (c >= 'A' && c <= 'Z') {
                flags = 0x02 | (c <= 'F' ? 0x01 : 0);
                lower[c + 128] = c + 32;
            } else if (c >= 'a' && c <= 'z') {
                flags = 0x10 | (c <= 'f' ? 0x01 : 0);
                upper[c + 128] = c - 32;
            } else
                flags = 0x08;
            classes[c + 128] = flags;
        }
    }
};
inline constexpr GuestCtypeTables GuestCtype{};
} // namespace Libraries::LibcInternal
