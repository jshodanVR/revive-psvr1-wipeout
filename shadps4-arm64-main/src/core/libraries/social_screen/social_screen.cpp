// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <cstring>

#include "common/logging/log.h"
#include "core/libraries/error_codes.h"
#include "core/libraries/libs.h"
#include "core/libraries/social_screen/social_screen.h"

namespace Libraries::SocialScreen {

// The emulator has no TV next to the headset, so every mode is accepted and nothing is shown.
// Titles still need these calls to succeed to get past their display setup.

s32 PS4_SYSV_ABI sceSocialScreenInitialize() {
    LOG_INFO(Lib_SocialScreen, "called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceSocialScreenTerminate() {
    LOG_INFO(Lib_SocialScreen, "called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceSocialScreenSetMode(s32 mode) {
    LOG_INFO(Lib_SocialScreen, "called, mode = {}", mode);
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI
sceSocialScreenInitializeSeparateModeParameter(OrbisSocialScreenSeparateModeParameter* param) {
    LOG_DEBUG(Lib_SocialScreen, "called");
    if (param != nullptr) {
        std::memset(param, 0, sizeof(OrbisSocialScreenSeparateModeParameter));
    }
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI
sceSocialScreenConfigureSeparateMode(const OrbisSocialScreenSeparateModeParameter* param) {
    LOG_INFO(Lib_SocialScreen, "called, mode = {}", param != nullptr ? param->mode : -1);
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceSocialScreenOpenSeparateMode() {
    LOG_INFO(Lib_SocialScreen, "called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceSocialScreenCloseSeparateMode() {
    LOG_INFO(Lib_SocialScreen, "called");
    return ORBIS_OK;
}

void RegisterLib(Core::Loader::SymbolsResolver* sym) {
    LIB_FUNCTION("pI7oFSPP65A", "libSceSocialScreen", 1, "libSceSocialScreen",
                 sceSocialScreenInitialize);
    LIB_FUNCTION("OVNpYTRqN74", "libSceSocialScreen", 1, "libSceSocialScreen",
                 sceSocialScreenTerminate);
    LIB_FUNCTION("6Me4hYsy3Kc", "libSceSocialScreen", 1, "libSceSocialScreen",
                 sceSocialScreenSetMode);
    LIB_FUNCTION("VMM7wQBZoBk", "libSceSocialScreen", 1, "libSceSocialScreen",
                 sceSocialScreenInitializeSeparateModeParameter);
    LIB_FUNCTION("IEzqdjIueps", "libSceSocialScreen", 1, "libSceSocialScreen",
                 sceSocialScreenConfigureSeparateMode);
    LIB_FUNCTION("SvdXHHt2LLE", "libSceSocialScreen", 1, "libSceSocialScreen",
                 sceSocialScreenOpenSeparateMode);
    LIB_FUNCTION("vtZIn9HtYbs", "libSceSocialScreen", 1, "libSceSocialScreen",
                 sceSocialScreenCloseSeparateMode);
};

} // namespace Libraries::SocialScreen
