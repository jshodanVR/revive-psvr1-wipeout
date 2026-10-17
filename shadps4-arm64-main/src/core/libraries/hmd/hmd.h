// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstddef>

#include "common/types.h"
#include "core/libraries/kernel/equeue.h"
#include "core/libraries/system/userservice.h"

namespace Core::Loader {
class SymbolsResolver;
}

namespace Libraries::Hmd {

enum OrbisHmdDeviceStatus : u32 {
    ORBIS_HMD_DEVICE_STATUS_READY,
    ORBIS_HMD_DEVICE_STATUS_NOT_READY,
    ORBIS_HMD_DEVICE_STATUS_NOT_DETECTED,
    ORBIS_HMD_DEVICE_STATUS_NOT_READY_HMU_DISCONNECT,
};

struct OrbisHmdInitializeParam {
    void* reserved0;
    u8 reserved[8];
};

struct OrbisHmdOpenParam {
    u8 reserve[32];
};

struct OrbisHmdFieldOfView {
    float tan_out;
    float tan_in;
    float tan_top;
    float tan_bottom;
};

struct OrbisHmdPanelResolution {
    u32 width;
    u32 height;
};

struct OrbisHmdFlipToDisplayLatency {
    u16 refresh_rate_90hz;
    u16 refresh_rate_120hz;
};

struct OrbisHmdDeviceInfo {
    OrbisHmdPanelResolution panel_resolution;
    OrbisHmdFlipToDisplayLatency flip_to_display_latency;
};

struct OrbisHmdDeviceInformation {
    OrbisHmdDeviceStatus status;
    Libraries::UserService::OrbisUserServiceUserId user_id;
    u8 reserve0[4];
    OrbisHmdDeviceInfo device_info;
    u8 hmu_mount;
    u8 reserve1[7];
};

struct OrbisHmdEyeOffset {
    float offset_x;
    float offset_y;
    float offset_z;
    u8 reserve[20];
};

struct OrbisHmdReprojectionResourceInfo {
    void* onion_buff;
    void* garlic_buff;
    s32 thread_priority;
    u32 padding;
    u64 cpu_affinity_mask;
    u32 pipe_id;
    u32 queue_id;
    u32 reserved[3];
};

/// Maps view-space tangents to texture coordinates for one eye: uv = tan * scale + offset.
struct OrbisHmdReprojectionEyeUv {
    float scale_x;
    float scale_y;
    float offset_x;
    float offset_y;
};

// The two structures below are not documented anywhere public. Their layout was recovered from
// how titles fill them in before calling sceHmdReprojectionStart; fields nothing has needed yet
// keep placeholder names.
struct OrbisHmdReprojectionParam {
    const void* texture[2]; ///< sce::Gnm::Texture of the left and right eye images.
    const void* sampler;    ///< sce::Gnm::Sampler used to read them.
    OrbisHmdReprojectionEyeUv uv[2];
    u64* frame_label; ///< Guest marks busy; reprojection clears after consuming the source images.
    u32 unknown_40;
    u32 padding0;
    u64 unknown_48;
    u32 unknown_50;
    u32 padding1;
    u8 flags;
};

/// Head pose the frame was rendered with, copied by the title from its tracker result.
struct OrbisHmdReprojectionTrackerState {
    float position[3];
    float orientation[4];
    u32 padding0;
    u64 timestamp;
    u64 sensor_read_system_timestamp;
    u32 user_frame_number;
    u32 padding1;
};

// Reprojection
// Observed single-layer ABI in Farpoint CUSA04508 01.08 (call at 0x3531d).
// Remaining fields and actual composition of multiple layers are not implemented.
struct OrbisHmdReprojectionLayer {
    const void* texture[2];
    u8 reserved_10[0x10];
    const void* sampler;
    OrbisHmdReprojectionEyeUv uv[2];
    u8 reserved_48[0x28];
    const void* transform;
    u8 reserved_78[0x30];
};
static_assert(sizeof(OrbisHmdReprojectionLayer) == 0xa8);
static_assert(offsetof(OrbisHmdReprojectionLayer, uv) == 0x28);
static_assert(offsetof(OrbisHmdReprojectionLayer, transform) == 0x70);

struct OrbisHmdReprojectionMultilayerInfo {
    u64* frame_label;
    u32 unknown_08;
    u32 padding0;
    u64 unknown_10;
    u32 unknown_18;
    u32 padding1;
    u64 flags;
};

s32 PS4_SYSV_ABI sceHmdReprojectionStartMultilayer(
    const OrbisHmdReprojectionLayer* layers, u32 count,
    const OrbisHmdReprojectionMultilayerInfo* info,
    const OrbisHmdReprojectionTrackerState* tracker_state, s64 flip_arg, s32 option);
s32 PS4_SYSV_ABI sceHmdReprojectionAddDisplayBuffer();
s32 PS4_SYSV_ABI sceHmdReprojectionClearUserEventEnd();
s32 PS4_SYSV_ABI sceHmdReprojectionClearUserEventStart();
s32 PS4_SYSV_ABI sceHmdReprojectionDebugGetLastInfo();
s32 PS4_SYSV_ABI sceHmdReprojectionDebugGetLastInfoMultilayer();
s32 PS4_SYSV_ABI sceHmdReprojectionFinalize();
s32 PS4_SYSV_ABI sceHmdReprojectionFinalizeCapture();
s32 PS4_SYSV_ABI sceHmdReprojectionInitialize(const OrbisHmdReprojectionResourceInfo* resource,
                                              s32 type, void* option);
s32 PS4_SYSV_ABI sceHmdReprojectionInitializeCapture();
s32 PS4_SYSV_ABI sceHmdReprojectionQueryGarlicBuffAlign();
s32 PS4_SYSV_ABI sceHmdReprojectionQueryGarlicBuffSize();
s32 PS4_SYSV_ABI sceHmdReprojectionQueryOnionBuffAlign();
s32 PS4_SYSV_ABI sceHmdReprojectionQueryOnionBuffSize();
s32 PS4_SYSV_ABI sceHmdReprojectionSetCallback();
s32 PS4_SYSV_ABI sceHmdReprojectionSetDisplayBuffers(s32 video_out_handle, s32 index0, s32 index1,
                                                     void* option);
s32 PS4_SYSV_ABI sceHmdReprojectionSetOutputMinColor(float red, float green, float blue);
s32 PS4_SYSV_ABI sceHmdReprojectionSetUserEventEnd(Libraries::Kernel::OrbisKernelEqueue eq,
                                                   s32 id);
s32 PS4_SYSV_ABI sceHmdReprojectionSetUserEventStart(Libraries::Kernel::OrbisKernelEqueue eq,
                                                     s32 id);
s32 PS4_SYSV_ABI sceHmdReprojectionStart(const OrbisHmdReprojectionParam* param,
                                         const OrbisHmdReprojectionTrackerState* tracker_state,
                                         s64 flip_arg, s32 option);
s32 PS4_SYSV_ABI sceHmdReprojectionStart2dVr();
s32 PS4_SYSV_ABI sceHmdReprojectionStartCapture();
s32 PS4_SYSV_ABI sceHmdReprojectionStartLiveCapture();
s32 PS4_SYSV_ABI sceHmdReprojectionStartMultilayer2();
s32 PS4_SYSV_ABI sceHmdReprojectionStartWideNear();
s32 PS4_SYSV_ABI sceHmdReprojectionStartWideNearWithOverlay();
s32 PS4_SYSV_ABI sceHmdReprojectionStartWithOverlay(const OrbisHmdReprojectionParam* param,
    const OrbisHmdReprojectionTrackerState* tracker_state, s64 flip_arg,
    const OrbisHmdReprojectionParam* overlay, s32 option);
s32 PS4_SYSV_ABI sceHmdReprojectionStop();
s32 PS4_SYSV_ABI sceHmdReprojectionStopCapture();
s32 PS4_SYSV_ABI sceHmdReprojectionStopLiveCapture();
s32 PS4_SYSV_ABI sceHmdReprojectionUnsetCallback();
s32 PS4_SYSV_ABI sceHmdReprojectionUnsetDisplayBuffers();

// Distortion
s32 PS4_SYSV_ABI sceHmdDistortionGet2dVrCommand();
s32 PS4_SYSV_ABI sceHmdDistortionGetCompoundEyeCorrectionCommand();
s32 PS4_SYSV_ABI sceHmdDistortionGetCorrectionCommand();
s32 PS4_SYSV_ABI sceHmdDistortionGetWideNearCorrectionCommand();
s32 PS4_SYSV_ABI sceHmdDistortionGetWorkMemoryAlign();
s32 PS4_SYSV_ABI sceHmdDistortionGetWorkMemorySize();
s32 PS4_SYSV_ABI sceHmdDistortionInitialize(void* reserved);
s32 PS4_SYSV_ABI sceHmdDistortionSetOutputMinColor();
s32 PS4_SYSV_ABI sceHmdDistortionTerminate();
s32 PS4_SYSV_ABI sceHmdGetDistortionParams();
s32 PS4_SYSV_ABI Func_B26430EA74FC3DC0();
s32 PS4_SYSV_ABI Func_B614F290B67FB59B();

// libSceHmd
s32 PS4_SYSV_ABI sceHmdClose(s32 handle);
s32 PS4_SYSV_ABI sceHmdGet2DEyeOffset(s32 handle, OrbisHmdEyeOffset* left_offset,
                                      OrbisHmdEyeOffset* right_offset);
s32 PS4_SYSV_ABI sceHmdGetAssyError(void* data);
s32 PS4_SYSV_ABI sceHmdGetDeviceInformation(OrbisHmdDeviceInformation* info);
s32 PS4_SYSV_ABI sceHmdGetDeviceInformationByHandle(s32 handle, OrbisHmdDeviceInformation* info);

s32 PS4_SYSV_ABI sceHmdGetFieldOfView(s32 handle, OrbisHmdFieldOfView* field_of_view);
s32 PS4_SYSV_ABI sceHmdGetInertialSensorData(s32 handle, void* data, s32 unk);
s32 PS4_SYSV_ABI sceHmdInitialize(const OrbisHmdInitializeParam* param);
s32 PS4_SYSV_ABI sceHmdInitialize315(const OrbisHmdInitializeParam* param);
s32 PS4_SYSV_ABI sceHmdOpen(Libraries::UserService::OrbisUserServiceUserId user_id, s32 type,
                            s32 index, OrbisHmdOpenParam* param);
s32 PS4_SYSV_ABI sceHmdTerminate();

// Internal
s32 PS4_SYSV_ABI sceHmdInternal3dAudioClose();
s32 PS4_SYSV_ABI sceHmdInternal3dAudioOpen();
s32 PS4_SYSV_ABI sceHmdInternal3dAudioSendData();
s32 PS4_SYSV_ABI sceHmdInternalAnotherScreenClose();
s32 PS4_SYSV_ABI sceHmdInternalAnotherScreenGetAudioStatus();
s32 PS4_SYSV_ABI sceHmdInternalAnotherScreenGetFadeState();
s32 PS4_SYSV_ABI sceHmdInternalAnotherScreenGetVideoStatus();
s32 PS4_SYSV_ABI sceHmdInternalAnotherScreenOpen();
s32 PS4_SYSV_ABI sceHmdInternalAnotherScreenSendAudio();
s32 PS4_SYSV_ABI sceHmdInternalAnotherScreenSendVideo();
s32 PS4_SYSV_ABI sceHmdInternalAnotherScreenSetFadeAndSwitch();
s32 PS4_SYSV_ABI sceHmdInternalBindDeviceWithUserId();
s32 PS4_SYSV_ABI sceHmdInternalCheckDeviceModelMk3();
s32 PS4_SYSV_ABI sceHmdInternalCheckS3dPassModeAvailable();
s32 PS4_SYSV_ABI sceHmdInternalCrashReportCancel();
s32 PS4_SYSV_ABI sceHmdInternalCrashReportClose();
s32 PS4_SYSV_ABI sceHmdInternalCrashReportOpen();
s32 PS4_SYSV_ABI sceHmdInternalCrashReportReadData();
s32 PS4_SYSV_ABI sceHmdInternalCrashReportReadDataSize();
s32 PS4_SYSV_ABI sceHmdInternalCreateSharedMemory();
s32 PS4_SYSV_ABI sceHmdInternalDfuCheckAfterPvt();
s32 PS4_SYSV_ABI sceHmdInternalDfuCheckPartialUpdateAvailable();
s32 PS4_SYSV_ABI sceHmdInternalDfuClose();
s32 PS4_SYSV_ABI sceHmdInternalDfuGetStatus();
s32 PS4_SYSV_ABI sceHmdInternalDfuOpen();
s32 PS4_SYSV_ABI sceHmdInternalDfuReset();
s32 PS4_SYSV_ABI sceHmdInternalDfuSend();
s32 PS4_SYSV_ABI sceHmdInternalDfuSendSize();
s32 PS4_SYSV_ABI sceHmdInternalDfuSetMode();
s32 PS4_SYSV_ABI sceHmdInternalDfuStart();
s32 PS4_SYSV_ABI sceHmdInternalEventInitialize();
s32 PS4_SYSV_ABI sceHmdInternalGetBrightness();
s32 PS4_SYSV_ABI sceHmdInternalGetCrashDumpInfo();
s32 PS4_SYSV_ABI sceHmdInternalGetDebugMode();
s32 PS4_SYSV_ABI sceHmdInternalGetDebugSocialScreenMode();
s32 PS4_SYSV_ABI sceHmdInternalGetDebugTextMode();
s32 PS4_SYSV_ABI sceHmdInternalGetDefaultLedData();
s32 PS4_SYSV_ABI sceHmdInternalGetDemoMode();
s32 PS4_SYSV_ABI sceHmdInternalGetDeviceInformation();
s32 PS4_SYSV_ABI sceHmdInternalGetDeviceInformationByHandle();
s32 PS4_SYSV_ABI sceHmdInternalGetDeviceStatus(OrbisHmdDeviceStatus* status);
s32 PS4_SYSV_ABI sceHmdInternalGetEyeStatus();
s32 PS4_SYSV_ABI sceHmdInternalGetHmuOpticalParam();
s32 PS4_SYSV_ABI sceHmdInternalGetHmuPowerStatusForDebug();
s32 PS4_SYSV_ABI sceHmdInternalGetHmuSerialNumber();
s32 PS4_SYSV_ABI sceHmdInternalGetIPD();
s32 PS4_SYSV_ABI sceHmdInternalGetIpdSettingEnableForSystemService();
s32 PS4_SYSV_ABI sceHmdInternalGetPuBuildNumber();
s32 PS4_SYSV_ABI sceHmdInternalGetPuPositionParam();
s32 PS4_SYSV_ABI sceHmdInternalGetPuRevision();
s32 PS4_SYSV_ABI sceHmdInternalGetPUSerialNumber();
s32 PS4_SYSV_ABI sceHmdInternalGetPUVersion();
s32 PS4_SYSV_ABI sceHmdInternalGetRequiredPUPVersion();
s32 PS4_SYSV_ABI sceHmdInternalGetStatusReport();
s32 PS4_SYSV_ABI sceHmdInternalGetTv4kCapability();
s32 PS4_SYSV_ABI sceHmdInternalGetVirtualDisplayDepth();
s32 PS4_SYSV_ABI sceHmdInternalGetVirtualDisplayHeight();
s32 PS4_SYSV_ABI sceHmdInternalGetVirtualDisplaySize();
s32 PS4_SYSV_ABI sceHmdInternalGetVr2dData();
s32 PS4_SYSV_ABI sceHmdInternalIsCommonDlgMiniAppVr2d();
s32 PS4_SYSV_ABI sceHmdInternalIsCommonDlgVr2d();
s32 PS4_SYSV_ABI sceHmdInternalIsGameVr2d();
s32 PS4_SYSV_ABI sceHmdInternalIsMiniAppVr2d();
s32 PS4_SYSV_ABI sceHmdInternalMapSharedMemory();
s32 PS4_SYSV_ABI sceHmdInternalMirroringModeSetAspect();
s32 PS4_SYSV_ABI sceHmdInternalMirroringModeSetAspectDebug();
s32 PS4_SYSV_ABI sceHmdInternalMmapGetCount();
s32 PS4_SYSV_ABI sceHmdInternalMmapGetModeId();
s32 PS4_SYSV_ABI sceHmdInternalMmapGetSensorCalibrationData();
s32 PS4_SYSV_ABI sceHmdInternalMmapIsConnect();
s32 PS4_SYSV_ABI sceHmdInternalPushVr2dData();
s32 PS4_SYSV_ABI sceHmdInternalRegisterEventCallback();
s32 PS4_SYSV_ABI sceHmdInternalResetInertialSensor();
s32 PS4_SYSV_ABI sceHmdInternalResetLedForVrTracker();
s32 PS4_SYSV_ABI sceHmdInternalResetLedForVsh();
s32 PS4_SYSV_ABI sceHmdInternalSeparateModeClose();
s32 PS4_SYSV_ABI sceHmdInternalSeparateModeGetAudioStatus();
s32 PS4_SYSV_ABI sceHmdInternalSeparateModeGetVideoStatus();
s32 PS4_SYSV_ABI sceHmdInternalSeparateModeOpen();
s32 PS4_SYSV_ABI sceHmdInternalSeparateModeSendAudio();
s32 PS4_SYSV_ABI sceHmdInternalSeparateModeSendVideo();
s32 PS4_SYSV_ABI sceHmdInternalSetBrightness();
s32 PS4_SYSV_ABI sceHmdInternalSetCrashReportCommand();
s32 PS4_SYSV_ABI sceHmdInternalSetDebugGpo();
s32 PS4_SYSV_ABI sceHmdInternalSetDebugMode();
s32 PS4_SYSV_ABI sceHmdInternalSetDebugSocialScreenMode();
s32 PS4_SYSV_ABI sceHmdInternalSetDebugTextMode();
s32 PS4_SYSV_ABI sceHmdInternalSetDefaultLedData();
s32 PS4_SYSV_ABI sceHmdInternalSetDemoMode();
s32 PS4_SYSV_ABI sceHmdInternalSetDeviceConnection();
s32 PS4_SYSV_ABI sceHmdInternalSetForcedCrash();
s32 PS4_SYSV_ABI sceHmdInternalSetHmuPowerControl();
s32 PS4_SYSV_ABI sceHmdInternalSetHmuPowerControlForDebug();
s32 PS4_SYSV_ABI sceHmdInternalSetIPD();
s32 PS4_SYSV_ABI sceHmdInternalSetIpdSettingEnableForSystemService();
s32 PS4_SYSV_ABI sceHmdInternalSetLedOn();
s32 PS4_SYSV_ABI sceHmdInternalSetM2LedBrightness();
s32 PS4_SYSV_ABI sceHmdInternalSetM2LedOn();
s32 PS4_SYSV_ABI sceHmdInternalSetPortConnection();
s32 PS4_SYSV_ABI sceHmdInternalSetPortStatus();
s32 PS4_SYSV_ABI sceHmdInternalSetS3dPassMode();
s32 PS4_SYSV_ABI sceHmdInternalSetSidetone();
s32 PS4_SYSV_ABI sceHmdInternalSetUserType();
s32 PS4_SYSV_ABI sceHmdInternalSetVirtualDisplayDepth();
s32 PS4_SYSV_ABI sceHmdInternalSetVirtualDisplayHeight();
s32 PS4_SYSV_ABI sceHmdInternalSetVirtualDisplaySize();
s32 PS4_SYSV_ABI sceHmdInternalSetVRMode();
s32 PS4_SYSV_ABI sceHmdInternalSocialScreenGetFadeState();
s32 PS4_SYSV_ABI sceHmdInternalSocialScreenSetFadeAndSwitch();
s32 PS4_SYSV_ABI sceHmdInternalSocialScreenSetOutput();

s32 PS4_SYSV_ABI Func_202D0D1A687FCD2F();
s32 PS4_SYSV_ABI Func_358DBF818A3D8A12();
s32 PS4_SYSV_ABI Func_5CCBADA76FE8F40E();
s32 PS4_SYSV_ABI Func_63D403167DC08CF0();
s32 PS4_SYSV_ABI Func_69383B2B4E3AEABF(s32 handle, void* data, s32 unk);
s32 PS4_SYSV_ABI Func_791560C32F4F6D68();
s32 PS4_SYSV_ABI Func_7C955961EA85B6D3();
s32 PS4_SYSV_ABI Func_9952277839236BA7();
s32 PS4_SYSV_ABI Func_9A276E739E54EEAF();
s32 PS4_SYSV_ABI Func_9E501994E289CBE7();
s32 PS4_SYSV_ABI Func_A31A0320D80EAD99();
s32 PS4_SYSV_ABI Func_A31F4DA8B3BD2E12();
s32 PS4_SYSV_ABI Func_A92D7C23AC364993();
s32 PS4_SYSV_ABI Func_ADCCC25CB876FDBE();
s32 PS4_SYSV_ABI Func_B16652641FE69F0E();
s32 PS4_SYSV_ABI Func_B9A6FA0735EC7E49();
s32 PS4_SYSV_ABI Func_FC193BD653F2AF2E();
s32 PS4_SYSV_ABI Func_FF2E0E53015FE231();

/// Called by the video output once per display refresh; the headset reprojects at that rate.
void OnVblank();

void RegisterDistortion(Core::Loader::SymbolsResolver* sym);
void RegisterReprojection(Core::Loader::SymbolsResolver* sym);
void RegisterLib(Core::Loader::SymbolsResolver* sym);
} // namespace Libraries::Hmd
