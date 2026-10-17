// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Minimal replacements for the settings/logging support the patch-domain library pulls in.
// The patch domain never reads user settings or config files; it only needs the log helpers
// to exist. Mirrored in the Android NDK build so the same C ABI is compiled for both.

#include <memory>
#include <string>

#include "core/emulator_settings.h"

EmulatorSettingsImpl::EmulatorSettingsImpl() = default;
EmulatorSettingsImpl::~EmulatorSettingsImpl() = default;

std::shared_ptr<EmulatorSettingsImpl> EmulatorSettingsImpl::GetInstance() {
    static const std::shared_ptr<EmulatorSettingsImpl> instance =
        std::make_shared<EmulatorSettingsImpl>();
    return instance;
}

void EmulatorSettingsImpl::SetInstance(std::shared_ptr<EmulatorSettingsImpl> /*instance*/) {}

namespace Common {

std::string GetCurrentThreadName() {
    return "shadPS4::PatchDomain";
}

} // namespace Common