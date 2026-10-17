// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <gtest/gtest.h>

#include "core/libraries/sysmodule/fios2_module_choice.h"

namespace {

using Libraries::SysModule::DecideFios2Module;
using Libraries::SysModule::Fios2ModuleChoice;

// CUSA00223 (inFAMOUS Second Son): no staged files, game ships libSceFios2.prx.
// HLE inline-completion hangs its SP engine thread, so the shipped module wins.
TEST(Fios2ModuleChoice, GameShippedPrxWinsOverHle) {
    EXPECT_EQ(DecideFios2Module(false, false, true), Fios2ModuleChoice::GameLle);
}

// A staged sys_modules module outranks the shipped one so fixes can be tested
// without touching the game dump.
TEST(Fios2ModuleChoice, StagedSprxWinsOverGamePrx) {
    EXPECT_EQ(DecideFios2Module(true, false, true), Fios2ModuleChoice::StagedLle);
}

// sys_modules/<serial>/libSceFios2.use-hle forces HLE even when LLE modules
// exist, for titles whose own Fios2 module misbehaves.
TEST(Fios2ModuleChoice, UseHleFlagOutranksEverything) {
    EXPECT_EQ(DecideFios2Module(true, true, true), Fios2ModuleChoice::Hle);
    EXPECT_EQ(DecideFios2Module(false, true, true), Fios2ModuleChoice::Hle);
}

TEST(Fios2ModuleChoice, DefaultsToHleWhenNothingShipped) {
    EXPECT_EQ(DecideFios2Module(false, false, false), Fios2ModuleChoice::Hle);
}

} // namespace
