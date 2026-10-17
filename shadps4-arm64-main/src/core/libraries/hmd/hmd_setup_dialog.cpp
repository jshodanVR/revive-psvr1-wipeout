// SPDX-FileCopyrightText: Copyright 2025 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "common/assert.h"
#include "common/logging/log.h"
#include "core/libraries/error_codes.h"
#include "core/libraries/hmd/hmd_setup_dialog.h"
#include "core/libraries/libs.h"
#include "core/vr/vr_runtime.h"
#include <atomic>
#include <chrono>
#include <cstdlib>

namespace Libraries::HmdSetupDialog {

// Opt-in compatibility probe. Statik polls this separate PSVR service dialog.
// Its observed GetResult caller allocates/clears 36 bytes and only needs success;
// write only the leading result code, without assuming undocumented trailing fields.
static std::atomic<s32> service_status{};
static std::chrono::steady_clock::time_point service_opened;
static s32 PS4_SYSV_ABI ServiceInitialize() { service_status = 1; return ORBIS_OK; }
static s32 PS4_SYSV_ABI ServiceOpen(const void* param) {
    if (!param) return -1;
    service_opened = std::chrono::steady_clock::now();
    service_status = 2;
    LOG_INFO(Lib_HmdSetupDialog, "Experimental VR service dialog opened for host headset");
    return ORBIS_OK;
}
static s32 PS4_SYSV_ABI ServiceStatus() {
    if (service_status == 2 && std::chrono::steady_clock::now() - service_opened >= std::chrono::milliseconds{100}) {
        service_status = 3;
        LOG_INFO(Lib_HmdSetupDialog, "Experimental VR service dialog finished");
    }
    return service_status;
}
static s32 PS4_SYSV_ABI ServiceResult(s32* result) {
    if (!result || service_status != 3) return -1;
    *result = static_cast<s32>(Core::Vr::Runtime::Instance().IsHeadsetConnected()
        ? Libraries::CommonDialog::Result::OK : Libraries::CommonDialog::Result::USER_CANCELED);
    return ORBIS_OK;
}
static s32 PS4_SYSV_ABI ServiceClose() { service_status = 3; return ORBIS_OK; }
static s32 PS4_SYSV_ABI ServiceTerminate() { service_status = 0; return ORBIS_OK; }

s32 PS4_SYSV_ABI sceHmdSetupDialogInitialize() {
    LOG_ERROR(Lib_HmdSetupDialog, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceHmdSetupDialogClose() {
    LOG_ERROR(Lib_HmdSetupDialog, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceHmdSetupDialogOpen(const OrbisHmdSetupDialogParam* param) {
    LOG_ERROR(Lib_HmdSetupDialog, "(STUBBED) called");
    // On real hardware, a dialog would show up telling the user to connect a PSVR headset.
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceHmdSetupDialogGetResult(OrbisHmdSetupDialogResult* result) {
    LOG_DEBUG(Lib_HmdSetupDialog, "called");
    // Result::OK means a headset was connected. Without one, simulate the user pressing circle
    // to cancel the dialog.
    result->result = Core::Vr::Runtime::Instance().IsHeadsetConnected()
                         ? Libraries::CommonDialog::Result::OK
                         : Libraries::CommonDialog::Result::USER_CANCELED;
    return ORBIS_OK;
}

Libraries::CommonDialog::Status PS4_SYSV_ABI sceHmdSetupDialogUpdateStatus() {
    LOG_ERROR(Lib_HmdSetupDialog, "(STUBBED) called");
    return Libraries::CommonDialog::Status::FINISHED;
}

Libraries::CommonDialog::Status PS4_SYSV_ABI sceHmdSetupDialogGetStatus() {
    LOG_ERROR(Lib_HmdSetupDialog, "(STUBBED) called");
    return Libraries::CommonDialog::Status::FINISHED;
}

s32 PS4_SYSV_ABI sceHmdSetupDialogTerminate() {
    LOG_ERROR(Lib_HmdSetupDialog, "(STUBBED) called");
    return ORBIS_OK;
}

void RegisterLib(Core::Loader::SymbolsResolver* sym) {
    if (std::getenv("SHADPS4_TEST_VR_SERVICE_DIALOG")) {
        LIB_FUNCTION("hYFXG8FWThI", "libSceVrServiceDialog", 1, "libSceVrServiceDialog", ServiceInitialize);
        LIB_FUNCTION("60-cjn5Dn0Q", "libSceVrServiceDialog", 1, "libSceVrServiceDialog", ServiceOpen);
        LIB_FUNCTION("RmRtBJpoHlA", "libSceVrServiceDialog", 1, "libSceVrServiceDialog", ServiceStatus);
        LIB_FUNCTION("kUavKmsczkY", "libSceVrServiceDialog", 1, "libSceVrServiceDialog", ServiceStatus);
        LIB_FUNCTION("cYnBkgm8I0c", "libSceVrServiceDialog", 1, "libSceVrServiceDialog", ServiceResult);
        LIB_FUNCTION("hBH2ABP7IeY", "libSceVrServiceDialog", 1, "libSceVrServiceDialog", ServiceClose);
        LIB_FUNCTION("M4xKWUytNMo", "libSceVrServiceDialog", 1, "libSceVrServiceDialog", ServiceTerminate);
    }
    LIB_FUNCTION("nmHzU4Gh0xs", "libSceHmdSetupDialog", 1, "libSceHmdSetupDialog",
                 sceHmdSetupDialogClose);
    LIB_FUNCTION("6lVRHMV5LY0", "libSceHmdSetupDialog", 1, "libSceHmdSetupDialog",
                 sceHmdSetupDialogGetResult);
    LIB_FUNCTION("J9eBpW1udl4", "libSceHmdSetupDialog", 1, "libSceHmdSetupDialog",
                 sceHmdSetupDialogGetStatus);
    LIB_FUNCTION("NB1Y2kA2jCY", "libSceHmdSetupDialog", 1, "libSceHmdSetupDialog",
                 sceHmdSetupDialogInitialize);
    LIB_FUNCTION("NNgiV4T+akU", "libSceHmdSetupDialog", 1, "libSceHmdSetupDialog",
                 sceHmdSetupDialogOpen);
    LIB_FUNCTION("+z4OJmFreZc", "libSceHmdSetupDialog", 1, "libSceHmdSetupDialog",
                 sceHmdSetupDialogTerminate);
    LIB_FUNCTION("Ud7j3+RDIBg", "libSceHmdSetupDialog", 1, "libSceHmdSetupDialog",
                 sceHmdSetupDialogUpdateStatus);
};

} // namespace Libraries::HmdSetupDialog
