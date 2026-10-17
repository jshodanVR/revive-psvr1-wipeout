// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

namespace Libraries::SysModule {

enum class Fios2ModuleChoice {
    StagedLle, ///< sys_modules/<serial>/libSceFios2.sprx
    GameLle,   ///< /app0/sce_module/libSceFios2.prx shipped by the game
    Hle,       ///< synchronous HLE implementation
};

// Decides which libSceFios2 implementation serves a title. Games driving the
// SP engine from an internal thread (inFAMOUS Second Son) block forever on the
// inline-complete HLE ops, so a shipped or staged LLE module is preferred. An
// explicit use-HLE flag outranks everything for titles whose own LLE module
// misbehaves (Bloodborne CUSA00900: LLE opens menu assets without reading them).
constexpr Fios2ModuleChoice DecideFios2Module(bool staged_sprx_exists, bool use_hle_flag_exists,
                                              bool game_prx_exists) {
    if (use_hle_flag_exists) {
        return Fios2ModuleChoice::Hle;
    }
    if (staged_sprx_exists) {
        return Fios2ModuleChoice::StagedLle;
    }
    if (game_prx_exists) {
        return Fios2ModuleChoice::GameLle;
    }
    return Fios2ModuleChoice::Hle;
}

} // namespace Libraries::SysModule
