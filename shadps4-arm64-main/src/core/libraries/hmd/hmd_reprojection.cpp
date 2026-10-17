// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <chrono>
#include <cmath>
#include <mutex>

#include "common/logging/log.h"
#include "core/guest_cpu/guest_watchdog.h"
#include "core/libraries/error_codes.h"
#include "core/libraries/hmd/hmd.h"
#include "core/libraries/hmd/hmd_error.h"
#include "core/libraries/libs.h"
#include "core/libraries/videoout/video_out.h"
#include "core/vr/vr_runtime.h"
#include "core/vr/hmd_overlay.h"

namespace Libraries::Hmd {

namespace {

struct UserEvent {
    Libraries::Kernel::OrbisKernelEqueue eq{};
    s32 id{};
    bool is_set{};

    void Trigger() const {
        if (!is_set) {
            return;
        }
        if (auto* equeue = Libraries::Kernel::GetEqueue(eq); equeue != nullptr) {
            equeue->TriggerEvent(id, Libraries::Kernel::OrbisKernelEvent::Filter::User, nullptr);
        }
    }
};

// On real hardware a system thread warps the latest submitted frame to the current head pose
// and scans it out on every panel refresh. Here the host compositor does the warping, so this
// only has to keep the guest-visible timing alive and pass submitted frames along.
struct Reprojection {
    std::mutex mutex;
    bool initialized{};
    s32 video_out_handle{-1};
    s32 display_index[2]{-1, -1};
    u32 submitted_frames{};
    UserEvent start_event;
    UserEvent end_event;
};

Reprojection g_reprojection;

Core::Vr::Fov FovFromUv(const OrbisHmdReprojectionEyeUv& left_eye) {
    if (!std::isfinite(left_eye.scale_x) || !std::isfinite(left_eye.scale_y) ||
        !std::isfinite(left_eye.offset_x) || !std::isfinite(left_eye.offset_y) ||
        left_eye.scale_x <= 0.0f || left_eye.scale_y <= 0.0f) {
        return Core::Vr::Runtime::Instance().GetConfig().fov;
    }
    // uv = tan * scale + offset, so the image edges (uv 0 and 1) give the half angles back.
    // On the left eye the left edge is the temple side.
    return {
        .tan_out = left_eye.offset_x / left_eye.scale_x,
        .tan_in = (1.0f - left_eye.offset_x) / left_eye.scale_x,
        .tan_top = left_eye.offset_y / left_eye.scale_y,
        .tan_bottom = (1.0f - left_eye.offset_y) / left_eye.scale_y,
    };
}

} // namespace

void OnVblank() {
    UserEvent start_event;
    UserEvent end_event;
    {
        std::scoped_lock lock{g_reprojection.mutex};
        if (!g_reprojection.initialized) {
            return;
        }
        start_event = g_reprojection.start_event;
        end_event = g_reprojection.end_event;
    }
    start_event.Trigger();
    end_event.Trigger();
}

s32 PS4_SYSV_ABI sceHmdReprojectionStartMultilayer(
    const OrbisHmdReprojectionLayer* layers, u32 count,
    const OrbisHmdReprojectionMultilayerInfo* info,
    const OrbisHmdReprojectionTrackerState* tracker_state, s64 flip_arg, s32 option) {
    // Opt-in while the additional per-layer transform and other layouts are investigated.
    if (!std::getenv("SHADPS4_TEST_HMD_MULTILAYER")) {
        LOG_ERROR(Lib_Hmd, "(STUBBED) called");
        return ORBIS_OK;
    }
    if (!layers || !info || !tracker_state) {
        return ORBIS_HMD_ERROR_PARAMETER_NULL;
    }
    if (count != 1) {
        LOG_ERROR(Lib_Hmd, "Unsupported multilayer count {}: no layers submitted", count);
        return ORBIS_HMD_ERROR_UNSUPPORTED_FEATURE;
    }
    static std::atomic<u32> calls{};
    if (calls.fetch_add(1) % 300 == 0) {
        LOG_INFO(Lib_Hmd, "Single-layer reprojection: textures={}/{} sampler={} transform={} "
                 "label={} unknown08={} unknown10={:#x} unknown18={:#x} flags={:#x}",
                 fmt::ptr(layers->texture[0]), fmt::ptr(layers->texture[1]),
                 fmt::ptr(layers->sampler), fmt::ptr(layers->transform),
                 fmt::ptr(info->frame_label), info->unknown_08, info->unknown_10,
                 info->unknown_18, info->flags);
    }
    OrbisHmdReprojectionParam param{};
    std::copy_n(layers->texture, 2, param.texture);
    param.sampler = layers->sampler;
    std::copy_n(layers->uv, 2, param.uv);
    param.frame_label = info->frame_label;
    param.unknown_40 = info->unknown_08;
    param.unknown_48 = info->unknown_10;
    param.unknown_50 = info->unknown_18;
    param.flags = static_cast<u8>(info->flags);
    return sceHmdReprojectionStart(&param, tracker_state, flip_arg, option);
}

s32 PS4_SYSV_ABI sceHmdReprojectionAddDisplayBuffer() {
    LOG_ERROR(Lib_Hmd, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceHmdReprojectionClearUserEventEnd() {
    LOG_INFO(Lib_Hmd, "called");
    std::scoped_lock lock{g_reprojection.mutex};
    g_reprojection.end_event = {};
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceHmdReprojectionClearUserEventStart() {
    LOG_INFO(Lib_Hmd, "called");
    std::scoped_lock lock{g_reprojection.mutex};
    g_reprojection.start_event = {};
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceHmdReprojectionDebugGetLastInfo() {
    LOG_ERROR(Lib_Hmd, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceHmdReprojectionDebugGetLastInfoMultilayer() {
    LOG_ERROR(Lib_Hmd, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceHmdReprojectionFinalize() {
    LOG_INFO(Lib_Hmd, "called");
    std::scoped_lock lock{g_reprojection.mutex};
    g_reprojection.initialized = false;
    g_reprojection.video_out_handle = -1;
    g_reprojection.start_event = {};
    g_reprojection.end_event = {};
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceHmdReprojectionFinalizeCapture() {
    LOG_ERROR(Lib_Hmd, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceHmdReprojectionInitialize(const OrbisHmdReprojectionResourceInfo* resource,
                                              s32 type, void* option) {
    if (resource == nullptr) {
        return ORBIS_HMD_ERROR_PARAMETER_NULL;
    }
    LOG_INFO(Lib_Hmd,
             "called, type = {}, thread_priority = {}, cpu_affinity_mask = {:#x}, pipe_id = {}, "
             "queue_id = {}",
             type, resource->thread_priority, resource->cpu_affinity_mask, resource->pipe_id,
             resource->queue_id);
    std::scoped_lock lock{g_reprojection.mutex};
    g_reprojection.initialized = true;
    g_reprojection.submitted_frames = 0;
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceHmdReprojectionInitializeCapture() {
    LOG_ERROR(Lib_Hmd, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceHmdReprojectionQueryGarlicBuffAlign() {
    return 0x100;
}

s32 PS4_SYSV_ABI sceHmdReprojectionQueryGarlicBuffSize() {
    return 0x100000;
}

s32 PS4_SYSV_ABI sceHmdReprojectionQueryOnionBuffAlign() {
    return 0x100;
}

s32 PS4_SYSV_ABI sceHmdReprojectionQueryOnionBuffSize() {
    return 0x810;
}

s32 PS4_SYSV_ABI sceHmdReprojectionSetCallback() {
    LOG_ERROR(Lib_Hmd, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceHmdReprojectionSetDisplayBuffers(s32 video_out_handle, s32 index0, s32 index1,
                                                     void* option) {
    LOG_INFO(Lib_Hmd, "called, video_out_handle = {}, index0 = {}, index1 = {}", video_out_handle,
             index0, index1);
    std::scoped_lock lock{g_reprojection.mutex};
    g_reprojection.video_out_handle = video_out_handle;
    g_reprojection.display_index[0] = index0;
    g_reprojection.display_index[1] = index1;
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceHmdReprojectionSetOutputMinColor(float red, float green, float blue) {
    LOG_DEBUG(Lib_Hmd, "called, red = {}, green = {}, blue = {}", red, green, blue);
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceHmdReprojectionSetUserEventEnd(Libraries::Kernel::OrbisKernelEqueue eq,
                                                   s32 id) {
    LOG_INFO(Lib_Hmd, "called, eq = {}, id = {}", eq, id);
    std::scoped_lock lock{g_reprojection.mutex};
    g_reprojection.end_event = {.eq = eq, .id = id, .is_set = true};
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceHmdReprojectionSetUserEventStart(Libraries::Kernel::OrbisKernelEqueue eq,
                                                     s32 id) {
    LOG_INFO(Lib_Hmd, "called, eq = {}, id = {}", eq, id);
    std::scoped_lock lock{g_reprojection.mutex};
    g_reprojection.start_event = {.eq = eq, .id = id, .is_set = true};
    return ORBIS_OK;
}

static s32 SubmitReprojection(const OrbisHmdReprojectionParam* param,
                                         const OrbisHmdReprojectionTrackerState* tracker_state,
                                         s64 flip_arg, s32 option, const OrbisHmdReprojectionParam* overlay) {
    if (param == nullptr || tracker_state == nullptr || param->texture[0] == nullptr ||
        param->texture[1] == nullptr) {
        return ORBIS_HMD_ERROR_PARAMETER_NULL;
    }

    Libraries::VideoOut::HmdFrame frame{};
    const char* swap_eyes = std::getenv("SHADPS4_VR_SWAP_EYES");
    frame.swap_eyes = swap_eyes && std::strcmp(swap_eyes, "1") == 0;
    const char* complete_label = std::getenv("SHADPS4_TEST_HMD_FRAME_LABEL");
    if (complete_label && std::strcmp(complete_label, "1") == 0) {
        frame.frame_label = param->frame_label;
    }
    std::memcpy(&frame.eye_textures[0], param->texture[0], sizeof(AmdGpu::Image));
    std::memcpy(&frame.eye_textures[1], param->texture[1], sizeof(AmdGpu::Image));
    auto left_uv = param->uv[0];
    frame.side_by_side = std::getenv("SHADPS4_TEST_HMD_SBS") &&
        frame.eye_textures[0].Address() == frame.eye_textures[1].Address() &&
        frame.eye_textures[0].width > 1 && ((frame.eye_textures[0].width + 1) % 2 == 0) &&
        param->uv[0].offset_x > 0.0f && param->uv[0].offset_x < 0.5f &&
        param->uv[1].offset_x > 0.5f && param->uv[1].offset_x < 1.0f;
    if (frame.side_by_side) {
        left_uv.scale_x *= 2.0f;
        left_uv.offset_x *= 2.0f;
    }
    if (overlay && overlay->texture[0] && overlay->texture[1]) {
        frame.overlay.enabled = true;
        for (int eye = 0; eye < 2; ++eye) {
            const auto& base = param->uv[eye];
            const auto& top = overlay->uv[eye];
            const auto uv = Core::Vr::OverlayTransform(
                {base.scale_x, base.scale_y, base.offset_x, base.offset_y},
                {top.scale_x, top.scale_y, top.offset_x, top.offset_y}, frame.side_by_side, eye);
            if (!uv) {
                frame.overlay.enabled = false;
                break;
            }
            std::memcpy(&frame.overlay.textures[eye], overlay->texture[eye], sizeof(AmdGpu::Image));
            frame.overlay.scale[eye] = uv->scale;
            frame.overlay.offset[eye] = uv->offset;
        }
    }
    frame.fov = FovFromUv(left_uv);
    frame.render_pose = {
        .position{tracker_state->position[0], tracker_state->position[1],
                  tracker_state->position[2]},
        .orientation{tracker_state->orientation[0], tracker_state->orientation[1],
                     tracker_state->orientation[2], tracker_state->orientation[3]},
    };
    frame.flip_arg = flip_arg;

    s32 video_out_handle;
    u32 frame_number;
    {
        std::scoped_lock lock{g_reprojection.mutex};
        if (!g_reprojection.initialized || g_reprojection.video_out_handle < 0) {
            return ORBIS_HMD_ERROR_NOT_INITIALIZED;
        }
        video_out_handle = g_reprojection.video_out_handle;
        frame_number = g_reprojection.submitted_frames++;
        // The reprojection alternates between the two display buffers it was given.
        frame.display_index = g_reprojection.display_index[frame_number & 1];
    }

    const bool trace_layout = std::getenv("SHADPS4_TRACE_HMD_LAYOUT") != nullptr;
    // Initial frames can use a dummy texture; sample after menus/overlay transitions too.
    if (frame_number < 4 || (trace_layout && frame_number % 300 == 0)) {
        LOG_INFO(Lib_Hmd, "Stereo texture layout: side_by_side={} swap_eyes={} uvL={}/{}/{}/{} uvR={}/{}/{}/{}",
                 frame.side_by_side, frame.swap_eyes, param->uv[0].scale_x, param->uv[0].scale_y,
                 param->uv[0].offset_x, param->uv[0].offset_y, param->uv[1].scale_x,
                 param->uv[1].scale_y, param->uv[1].offset_x, param->uv[1].offset_y);
        const auto& left = frame.eye_textures[0];
        LOG_INFO(Lib_Hmd,
                 "frame {}: flip_arg = {}, eye {}x{} at {:#x} / {:#x}, tan out/in/top/bottom = "
                 "{:.4f}/{:.4f}/{:.4f}/{:.4f}, unknown_40 = {:#x}, flags = {:#x}, option = {}",
                 frame_number, flip_arg, left.width + 1, left.height + 1, left.Address(),
                 frame.eye_textures[1].Address(), frame.fov.tan_out, frame.fov.tan_in,
                 frame.fov.tan_top, frame.fov.tan_bottom, param->unknown_40, param->flags, option);
        LOG_INFO(Lib_Hmd, "Render pose position={}/{}/{} orientation={}/{}/{}/{}",
                 frame.render_pose.position.x, frame.render_pose.position.y,
                 frame.render_pose.position.z, frame.render_pose.orientation.x,
                 frame.render_pose.orientation.y, frame.render_pose.orientation.z,
                 frame.render_pose.orientation.w);
        LOG_INFO(Lib_Hmd,
                 "eye UV: left scale {:.6f}/{:.6f}, offset {:.6f}/{:.6f}; "
                 "right scale {:.6f}/{:.6f}, offset {:.6f}/{:.6f}",
                 param->uv[0].scale_x, param->uv[0].scale_y, param->uv[0].offset_x,
                 param->uv[0].offset_y, param->uv[1].scale_x, param->uv[1].scale_y,
                 param->uv[1].offset_x, param->uv[1].offset_y);
    }

    Core::GuestCpu::NoteGuestProgress();

    // How fast the title delivers frames is the number that matters for comfort; report it now
    // and then.
    {
        using Clock = std::chrono::steady_clock;
        static constexpr auto ReportInterval = std::chrono::seconds{10};
        static Clock::time_point report_time = Clock::now();
        static u32 report_frame = 0;
        const auto now = Clock::now();
        if (now - report_time >= ReportInterval) {
            const float seconds = std::chrono::duration<float>(now - report_time).count();
            LOG_INFO(Lib_Hmd, "{} headset frames so far, {:.1f} per second", frame_number,
                     static_cast<float>(frame_number - report_frame) / seconds);
            report_time = now;
            report_frame = frame_number;
        }
    }

    const s32 result = Libraries::VideoOut::SubmitHmdFrame(video_out_handle, frame);
    if (result != ORBIS_OK) {
        LOG_ERROR(Lib_Hmd, "Could not queue headset frame: {:#x}", static_cast<u32>(result));
    }
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceHmdReprojectionStart(const OrbisHmdReprojectionParam* param,
    const OrbisHmdReprojectionTrackerState* tracker_state, s64 flip_arg, s32 option) {
    return SubmitReprojection(param, tracker_state, flip_arg, option, nullptr);
}

s32 PS4_SYSV_ABI sceHmdReprojectionStart2dVr() {
    LOG_ERROR(Lib_Hmd, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceHmdReprojectionStartCapture() {
    LOG_ERROR(Lib_Hmd, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceHmdReprojectionStartLiveCapture() {
    LOG_ERROR(Lib_Hmd, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceHmdReprojectionStartMultilayer2() {
    LOG_ERROR(Lib_Hmd, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceHmdReprojectionStartWideNear() {
    LOG_ERROR(Lib_Hmd, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceHmdReprojectionStartWideNearWithOverlay() {
    LOG_ERROR(Lib_Hmd, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceHmdReprojectionStartWithOverlay(const OrbisHmdReprojectionParam* param,
    const OrbisHmdReprojectionTrackerState* tracker_state, s64 flip_arg,
    const OrbisHmdReprojectionParam* overlay, s32 option) {
    if (std::getenv("SHADPS4_TEST_HMD_BASE_LAYER")) {
        static std::atomic<bool> reported{};
        if (!reported.exchange(true)) {
            LOG_WARNING(Lib_Hmd, "Overlay reprojection enabled; overlay={}",
                        fmt::ptr(overlay));
        }
        if (std::getenv("SHADPS4_TRACE_HMD_LAYOUT")) {
            static std::atomic<u32> overlay_calls{};
            if (overlay_calls.fetch_add(1) % 300 == 0 && overlay) {
                LOG_INFO(Lib_Hmd, "Overlay parameters textures={}/{} uvL={}/{}/{}/{} "
                         "uvR={}/{}/{}/{} flags={:#x}", fmt::ptr(overlay->texture[0]),
                         fmt::ptr(overlay->texture[1]), overlay->uv[0].scale_x,
                         overlay->uv[0].scale_y, overlay->uv[0].offset_x, overlay->uv[0].offset_y,
                         overlay->uv[1].scale_x, overlay->uv[1].scale_y,
                         overlay->uv[1].offset_x, overlay->uv[1].offset_y, overlay->flags);
            }
        }
        // Diagnostic only: inspect the omitted layer using the normal synchronized
        // texture/presentation path. This is NOT overlay composition.
        if (std::getenv("SHADPS4_TEST_HMD_OVERLAY_ONLY") && overlay &&
            overlay->texture[0] && overlay->texture[1]) {
            return sceHmdReprojectionStart(overlay, tracker_state, flip_arg, option);
        }
        const char* base_only = std::getenv("SHADPS4_TEST_HMD_OVERLAY_BASE_ONLY");
        return SubmitReprojection(param, tracker_state, flip_arg, option,
                                  base_only ? nullptr : overlay);
    }
    LOG_ERROR(Lib_Hmd, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceHmdReprojectionStop() {
    LOG_INFO(Lib_Hmd, "called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceHmdReprojectionStopCapture() {
    LOG_ERROR(Lib_Hmd, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceHmdReprojectionStopLiveCapture() {
    LOG_ERROR(Lib_Hmd, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceHmdReprojectionUnsetCallback() {
    LOG_ERROR(Lib_Hmd, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceHmdReprojectionUnsetDisplayBuffers() {
    LOG_INFO(Lib_Hmd, "called");
    std::scoped_lock lock{g_reprojection.mutex};
    g_reprojection.video_out_handle = -1;
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI Func_A31A0320D80EAD99() {
    LOG_ERROR(Lib_Hmd, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI Func_B9A6FA0735EC7E49() {
    LOG_ERROR(Lib_Hmd, "(STUBBED) called");
    return ORBIS_OK;
}

void RegisterReprojection(Core::Loader::SymbolsResolver* sym) {
    LIB_FUNCTION("8gH1aLgty5I", "libsceHmdReprojectionMultilayer", 1, "libSceHmd",
                 sceHmdReprojectionStartMultilayer);
    LIB_FUNCTION("NTIbBpSH9ik", "libSceHmd", 1, "libSceHmd", sceHmdReprojectionAddDisplayBuffer);
    LIB_FUNCTION("94+Ggm38KCg", "libSceHmd", 1, "libSceHmd", sceHmdReprojectionClearUserEventEnd);
    LIB_FUNCTION("mdyFbaJj66M", "libSceHmd", 1, "libSceHmd", sceHmdReprojectionClearUserEventStart);
    LIB_FUNCTION("MdV0akauNow", "libSceHmd", 1, "libSceHmd", sceHmdReprojectionDebugGetLastInfo);
    LIB_FUNCTION("ymiwVjPB5+k", "libSceHmd", 1, "libSceHmd",
                 sceHmdReprojectionDebugGetLastInfoMultilayer);
    LIB_FUNCTION("ZrV5YIqD09I", "libSceHmd", 1, "libSceHmd", sceHmdReprojectionFinalize);
    LIB_FUNCTION("utHD2Ab-Ixo", "libSceHmd", 1, "libSceHmd", sceHmdReprojectionFinalizeCapture);
    LIB_FUNCTION("OuygGEWkins", "libSceHmd", 1, "libSceHmd", sceHmdReprojectionInitialize);
    LIB_FUNCTION("BTrQnC6fcAk", "libSceHmd", 1, "libSceHmd", sceHmdReprojectionInitializeCapture);
    LIB_FUNCTION("TkcANcGM0s8", "libSceHmd", 1, "libSceHmd",
                 sceHmdReprojectionQueryGarlicBuffAlign);
    LIB_FUNCTION("z0KtN1vqF2E", "libSceHmd", 1, "libSceHmd", sceHmdReprojectionQueryGarlicBuffSize);
    LIB_FUNCTION("IWybWbR-xvA", "libSceHmd", 1, "libSceHmd", sceHmdReprojectionQueryOnionBuffAlign);
    LIB_FUNCTION("kLUAkN6a1e8", "libSceHmd", 1, "libSceHmd", sceHmdReprojectionQueryOnionBuffSize);
    LIB_FUNCTION("6CRWGc-evO4", "libSceHmd", 1, "libSceHmd", sceHmdReprojectionSetCallback);
    LIB_FUNCTION("E+dPfjeQLHI", "libSceHmd", 1, "libSceHmd", sceHmdReprojectionSetDisplayBuffers);
    LIB_FUNCTION("LjdLRysHU6Y", "libSceHmd", 1, "libSceHmd", sceHmdReprojectionSetOutputMinColor);
    LIB_FUNCTION("knyIhlkpLgE", "libSceHmd", 1, "libSceHmd", sceHmdReprojectionSetUserEventEnd);
    LIB_FUNCTION("7as0CjXW1B8", "libSceHmd", 1, "libSceHmd", sceHmdReprojectionSetUserEventStart);
    LIB_FUNCTION("dntZTJ7meIU", "libSceHmd", 1, "libSceHmd", sceHmdReprojectionStart);
    LIB_FUNCTION("q3e8+nEguyE", "libSceHmd", 1, "libSceHmd", sceHmdReprojectionStart2dVr);
    LIB_FUNCTION("RrvyU1pjb9A", "libSceHmd", 1, "libSceHmd", sceHmdReprojectionStartCapture);
    LIB_FUNCTION("XZ5QUzb4ae0", "libSceHmd", 1, "libSceHmd", sceHmdReprojectionStartLiveCapture);
    LIB_FUNCTION("8gH1aLgty5I", "libSceHmd", 1, "libSceHmd", sceHmdReprojectionStartMultilayer);
    LIB_FUNCTION("gqAG7JYeE7A", "libSceHmd", 1, "libSceHmd", sceHmdReprojectionStartMultilayer2);
    LIB_FUNCTION("3JyuejcNhC0", "libSceHmd", 1, "libSceHmd", sceHmdReprojectionStartWideNear);
    LIB_FUNCTION("mKa8scOc4-k", "libSceHmd", 1, "libSceHmd",
                 sceHmdReprojectionStartWideNearWithOverlay);
    LIB_FUNCTION("kcldQ7zLYQQ", "libSceHmd", 1, "libSceHmd", sceHmdReprojectionStartWithOverlay);
    LIB_FUNCTION("vzMEkwBQciM", "libSceHmd", 1, "libSceHmd", sceHmdReprojectionStop);
    LIB_FUNCTION("F7Sndm5teWw", "libSceHmd", 1, "libSceHmd", sceHmdReprojectionStopCapture);
    LIB_FUNCTION("PAa6cUL5bR4", "libSceHmd", 1, "libSceHmd", sceHmdReprojectionStopLiveCapture);
    LIB_FUNCTION("0wnZViigP9o", "libSceHmd", 1, "libSceHmd", sceHmdReprojectionUnsetCallback);
    LIB_FUNCTION("iGNNpDDjcwo", "libSceHmd", 1, "libSceHmd", sceHmdReprojectionUnsetDisplayBuffers);
    LIB_FUNCTION("oxoDINgOrZk", "libSceHmd", 1, "libSceHmd", Func_A31A0320D80EAD99);
    LIB_FUNCTION("uab6BzXsfkk", "libSceHmd", 1, "libSceHmd", Func_B9A6FA0735EC7E49);
}
} // namespace Libraries::Hmd
