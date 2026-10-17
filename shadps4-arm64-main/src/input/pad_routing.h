// SPDX-License-Identifier: GPL-2.0-or-later
// Adapted from evertec82/Farpoint, de6d8df.
#pragma once
namespace Input {
constexpr bool PlayerUsesPadPort(bool farpoint, bool special, int port) {
    // Farpoint keeps a standard port for menus/calibration alongside the Aim port.
    return port == (special ? 2 : 0) || (farpoint && special && port == 0);
}
constexpr bool PadPortIsSpecial(bool farpoint, bool special, int port) {
    return special && (!farpoint || port == 2);
}
} // namespace Input
