// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "common/types.h"
#include "core/libraries/system/userservice.h"

namespace Core::Loader {
class SymbolsResolver;
}

namespace Libraries::Move {

// Logical Move masks, matching the controller API (not the raw USB report).
enum Button : u16 { Select = 1, Trigger = 2, Move = 4, Start = 8,
                    Triangle = 16, Circle = 32, Cross = 64, Square = 128 };
// Host/tracker bridge. Returns -1 for a stale or unknown handle.
s32 HandForHandle(s32 handle);

struct OrbisMoveDeviceInfo {
    float sphere_radius;
    float accelerometer_offset[3];
};

struct OrbisMoveButtonData {
    u16 button_data;
    u16 trigger_data;
};

struct OrbisMoveExtensionPortData {
    u16 status;
    u16 digital0;
    u16 digital1;
    u16 analog_right_x;
    u16 analog_right_y;
    u16 analog_left_x;
    u16 analog_left_y;
    unsigned char custom[5];
};

struct OrbisMoveData {
    float accelerometer[3];
    float gyro[3];
    OrbisMoveButtonData button_data;
    OrbisMoveExtensionPortData extension_data;
    s64 timestamp;
    s32 count;
    float temperature;
};

void RegisterLib(Core::Loader::SymbolsResolver* sym);
} // namespace Libraries::Move
