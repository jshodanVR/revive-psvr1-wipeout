// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <string>
#include <string_view>

namespace Libraries::AvPlayer {

inline std::string AvPlayerNormalizeGuestPath(std::string_view path) {
    std::string out(path);
    for (char& ch : out) {
        if (ch == '\\') {
            ch = '/';
        }
    }
    return out;
}

} // namespace Libraries::AvPlayer
