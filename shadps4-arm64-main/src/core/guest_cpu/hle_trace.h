// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

namespace Core::GuestCpu {

// BACHATA_FEX_HLE_TRACE stderr dump. Off unless the env value is present and
// not empty/"0". Hot HLE (memcpy/locks) hits this path tens of thousands of
// times per minute; default-on fprintf cut Driveclub 3D to 11-15 fps.
inline bool BachataHleTraceRequested(const char* value) {
    return value != nullptr && value[0] != '\0' && value[0] != '0';
}

} // namespace Core::GuestCpu
