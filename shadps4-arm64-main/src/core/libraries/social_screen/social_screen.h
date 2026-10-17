// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "common/types.h"

namespace Core::Loader {
class SymbolsResolver;
}

namespace Libraries::SocialScreen {

// What the TV shows while a headset is in use: a mirror of the headset view, or an image the
// title renders separately through the social screen video output bus.
struct OrbisSocialScreenSeparateModeParameter {
    s32 mode;
    u8 reserved[28];
};

s32 PS4_SYSV_ABI sceSocialScreenInitialize();
s32 PS4_SYSV_ABI sceSocialScreenTerminate();
s32 PS4_SYSV_ABI sceSocialScreenSetMode(s32 mode);
s32 PS4_SYSV_ABI
sceSocialScreenInitializeSeparateModeParameter(OrbisSocialScreenSeparateModeParameter* param);
s32 PS4_SYSV_ABI
sceSocialScreenConfigureSeparateMode(const OrbisSocialScreenSeparateModeParameter* param);
s32 PS4_SYSV_ABI sceSocialScreenOpenSeparateMode();
s32 PS4_SYSV_ABI sceSocialScreenCloseSeparateMode();

void RegisterLib(Core::Loader::SymbolsResolver* sym);
} // namespace Libraries::SocialScreen
