// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cmath>

namespace Input {
// A scarce Menu button doubles as a modifier. A plain press/release is Options;
// using either stick while held selects D-pad/touchpad without opening the menu.
class VrGamepadLayer {
public:
    struct State { bool alternate; bool options; };
    State Update(bool enabled, bool menu, float lx, float ly, float rx, float ry,
                 bool left_click, bool right_click) {
        if (!enabled) { Reset(); return {false, menu}; }
        if (menu && !held) used = false;
        if (menu && (std::abs(lx) > .2f || std::abs(ly) > .2f ||
                     std::abs(rx) > .2f || std::abs(ry) > .2f || left_click || right_click))
            used = true;
        const bool options = held && !menu && !used;
        held = menu;
        return {menu, options};
    }
    void Reset() { held = used = false; }
private:
    bool held{};
    bool used{};
};
} // namespace Input
