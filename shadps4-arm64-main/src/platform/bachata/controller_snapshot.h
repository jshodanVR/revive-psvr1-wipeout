// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

namespace Platform::Bachata {

struct ControllerSnapshot {
    int slot{};
    std::uint64_t sequence{};
    std::uint64_t buttons{};
    int left_x = 128;
    int left_y = 128;
    int right_x = 128;
    int right_y = 128;
    int left_trigger{};
    int right_trigger{};
    bool touch_down{};
    int touch_x{};
    int touch_y{};
    // Optional DS4 motion payload (gyro rad/s, accel m/s² — same units SDL reports).
    bool has_motion{};
    float gyro[3]{};
    float accel[3]{};
};

std::optional<ControllerSnapshot> ParseControllerSnapshot(std::string_view line);

} // namespace Platform::Bachata
