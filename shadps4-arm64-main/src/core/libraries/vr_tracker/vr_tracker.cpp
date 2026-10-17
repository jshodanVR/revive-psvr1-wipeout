// SPDX-FileCopyrightText: Copyright 2025 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <atomic>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <vector>

#include "common/logging/log.h"
#include "core/libraries/error_codes.h"
#include "core/libraries/kernel/time.h"
#include "core/libraries/move/move.h"
#include "core/libraries/libs.h"
#include "core/libraries/vr_tracker/calibration_state.h"
#include "core/libraries/vr_tracker/init_layout.h"
#include "core/libraries/vr_tracker/mapped_range.h"
#include "core/libraries/vr_tracker/vr_tracker.h"
#include "core/known_title.h"
#include "core/libraries/vr_tracker/vr_tracker_error.h"
#include "core/memory.h"
#include "core/vr/vr_runtime.h"
#include "video_core/amdgpu/liverpool.h"

namespace Libraries::VrTracker {

static bool g_library_initialized = false;

// Internal memory
static void* g_garlic_memory_pointer = nullptr;
static u32 g_garlic_size = 0;
static void* g_onion_memory_pointer = nullptr;
static u32 g_onion_size = 0;
static void* g_work_memory_pointer = nullptr;
static u32 g_work_size = 0;

// Registered handles
// The DualShock 4s registered, in the order they were: the first is the player's, the one the
// host tracks. A console's tracker tells up to four apart by the colour of their light bars;
// a title registers the controller of every player logged in.
struct PadRegistration {
    s32 handle;
    OrbisVrTrackerLedColor color;
};
static std::vector<PadRegistration> g_pads;
static std::mutex g_pads_mutex;
static std::unordered_map<s32, s32> g_moves;
static std::mutex g_moves_mutex;
static std::optional<s32> FindMove(s32 handle) {
    std::scoped_lock lock{g_moves_mutex};
    const auto found = g_moves.find(handle);
    return found == g_moves.end() ? std::nullopt : std::optional{found->second};
}
static s32 g_gun_handle = -1;
static s32 g_hmd_handle = -1;
static std::mutex g_calibration_mutex;
static std::unordered_map<s32, CalibrationState> g_calibrations;

/// The registration of a DualShock 4 handle, if it is one.
static std::optional<PadRegistration> FindPad(s32 handle) {
    std::scoped_lock lock{g_pads_mutex};
    for (const PadRegistration& pad : g_pads) {
        if (pad.handle == handle) {
            return pad;
        }
    }
    return std::nullopt;
}

/// Whether a handle is the player's controller, the one the host tracks.
static bool IsPlayersPad(s32 handle) {
    std::scoped_lock lock{g_pads_mutex};
    return !g_pads.empty() && g_pads.front().handle == handle;
}

static bool HeadsetConnected() {
    return Core::Vr::Runtime::Instance().IsHeadsetConnected();
}

static void WritePose(OrbisVrTrackerPoseData& out, const Core::Vr::Pose& pose) {
    out.position_x = pose.position.x;
    out.position_y = pose.position.y;
    out.position_z = pose.position.z;
    out.orientation_x = pose.orientation.x;
    out.orientation_y = pose.orientation.y;
    out.orientation_z = pose.orientation.z;
    out.orientation_w = pose.orientation.w;
}

s32 PS4_SYSV_ABI sceVrTrackerQueryMemory(const OrbisVrTrackerQueryMemoryParam* param,
                                         OrbisVrTrackerQueryMemoryResult* result) {
    LOG_DEBUG(Lib_VrTracker, "called");
    if (param == nullptr || result == nullptr ||
        param->size != sizeof(OrbisVrTrackerQueryMemoryParam) ||
        (param->profile != OrbisVrTrackerProfile::ORBIS_VR_TRACKER_PROFILE_000 &&
         param->profile != OrbisVrTrackerProfile::ORBIS_VR_TRACKER_PROFILE_100) ||
        param->calibration_settings.pad_position >
            OrbisVrTrackerCalibrationMode::ORBIS_VR_TRACKER_CALIBRATION_AUTO ||
        // Hmd doesn't support auto calibration
        param->calibration_settings.hmd_position >
            OrbisVrTrackerCalibrationMode::ORBIS_VR_TRACKER_CALIBRATION_MANUAL ||
        param->calibration_settings.move_position >
            OrbisVrTrackerCalibrationMode::ORBIS_VR_TRACKER_CALIBRATION_AUTO ||
        param->calibration_settings.gun_position >
            OrbisVrTrackerCalibrationMode::ORBIS_VR_TRACKER_CALIBRATION_AUTO) {
        return ORBIS_VR_TRACKER_ERROR_ARGUMENT_INVALID;
    }

    // Setting move_position to ORBIS_VR_TRACKER_CALIBRATION_AUTO doubles required onion memory.
    u32 required_onion_size = ORBIS_VR_TRACKER_BASE_ONION_SIZE;
    if (param->calibration_settings.move_position ==
        OrbisVrTrackerCalibrationMode::ORBIS_VR_TRACKER_CALIBRATION_AUTO) {
        required_onion_size *= 2;
    }

    result->direct_memory_onion_size = required_onion_size;
    result->direct_memory_onion_alignment = ORBIS_VR_TRACKER_MEMORY_ALIGNMENT;
    result->direct_memory_garlic_size = ORBIS_VR_TRACKER_GARLIC_SIZE;
    result->direct_memory_garlic_alignment = ORBIS_VR_TRACKER_MEMORY_ALIGNMENT;
    result->work_memory_size = ORBIS_VR_TRACKER_WORK_SIZE;
    result->work_memory_alignment = ORBIS_VR_TRACKER_MEMORY_ALIGNMENT;

    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceVrTrackerInit(const OrbisVrTrackerInitParam* param) {
    OrbisVrTrackerInitParam normalized{};
    if (!NormalizeInit(param, normalized)) return ORBIS_VR_TRACKER_ERROR_ARGUMENT_INVALID;
    const u32 input_size = param->size;
    param = &normalized;
    LOG_INFO(Lib_VrTracker, "Tracker init size={}/{} garlic={}/{}/{} onion={}/{}/{} "
             "work={}/{}/{} pipe={} queue={} calibration={}/{}/{}/{}",
             input_size, sizeof(OrbisVrTrackerInitParam), fmt::ptr(param->direct_memory_garlic),
             param->direct_memory_garlic_size, param->direct_memory_garlic_alignment,
             fmt::ptr(param->direct_memory_onion), param->direct_memory_onion_size,
             param->direct_memory_onion_alignment, fmt::ptr(param->work_memory),
             param->work_memory_size, param->work_memory_alignment, param->gpu_pipe_id,
             param->gpu_queue_id, static_cast<u32>(param->calibration_settings.pad_position),
             static_cast<u32>(param->calibration_settings.hmd_position),
             static_cast<u32>(param->calibration_settings.move_position),
             static_cast<u32>(param->calibration_settings.gun_position));
    if (g_library_initialized) {
        return ORBIS_VR_TRACKER_ERROR_ALREADY_INITIALIZED;
    }

    // Calculate correct onion size for parameter checks
    u32 required_onion_size = ORBIS_VR_TRACKER_BASE_ONION_SIZE;
    if (param->calibration_settings.move_position == ORBIS_VR_TRACKER_CALIBRATION_AUTO) {
        required_onion_size *= 2;
    }

    // Parameter checks are fairly thorough here.
    if (param->size != sizeof(OrbisVrTrackerInitParam) ||
        // Check garlic memory parameters
        param->direct_memory_garlic == nullptr ||
        param->direct_memory_garlic_alignment != ORBIS_VR_TRACKER_MEMORY_ALIGNMENT ||
        param->direct_memory_garlic_size != ORBIS_VR_TRACKER_GARLIC_SIZE ||
        // Check onion memory parameters
        param->direct_memory_onion == nullptr ||
        param->direct_memory_onion_alignment != ORBIS_VR_TRACKER_MEMORY_ALIGNMENT ||
        param->direct_memory_onion_size != required_onion_size ||
        // Check work memory parameters
        param->work_memory == nullptr ||
        param->work_memory_alignment != ORBIS_VR_TRACKER_MEMORY_ALIGNMENT ||
        param->work_memory_size != ORBIS_VR_TRACKER_WORK_SIZE ||
        // Check compute queue parameters
        param->gpu_pipe_id >= AmdGpu::Liverpool::NumComputePipes ||
        param->gpu_queue_id >= AmdGpu::Liverpool::NumQueuesPerPipe ||
        // Check calibration settings
        param->calibration_settings.pad_position >
            OrbisVrTrackerCalibrationMode::ORBIS_VR_TRACKER_CALIBRATION_AUTO ||
        param->calibration_settings.hmd_position >
            OrbisVrTrackerCalibrationMode::ORBIS_VR_TRACKER_CALIBRATION_MANUAL ||
        param->calibration_settings.move_position >
            OrbisVrTrackerCalibrationMode::ORBIS_VR_TRACKER_CALIBRATION_AUTO ||
        param->calibration_settings.gun_position >
            OrbisVrTrackerCalibrationMode::ORBIS_VR_TRACKER_CALIBRATION_AUTO) {
        return ORBIS_VR_TRACKER_ERROR_ARGUMENT_INVALID;
    }

    // Real hardware will segfault if any of the supplied mappings aren't long enough,
    // Validate each of them to ensure nothing weird can occur when this library is implemented.
    auto* memory = Core::Memory::Instance();
    const auto query = [&](std::uintptr_t address) -> std::optional<MappedSpan> {
        Libraries::Kernel::OrbisVirtualQueryInfo info{};
        if (memory->VirtualQuery(address, 0, &info) != 0)
            return std::nullopt;
        return MappedSpan{info.start, info.end, bool(info.is_committed),
                          info.memory_type, info.protection};
    };
    // Check every mapping, including independently mapped adjacent chunks.
    VAddr addr_to_check = std::bit_cast<VAddr>(param->direct_memory_garlic);
    ASSERT_MSG(ValidateMappedRange(addr_to_check, param->direct_memory_garlic_size, query),
               "Insufficient garlic memory provided");

    g_garlic_memory_pointer = param->direct_memory_garlic;
    g_garlic_size = param->direct_memory_garlic_size;

    addr_to_check = std::bit_cast<VAddr>(param->direct_memory_onion);
    ASSERT_MSG(ValidateMappedRange(addr_to_check, param->direct_memory_onion_size, query),
               "Insufficient onion memory provided");

    g_onion_memory_pointer = param->direct_memory_onion;
    g_onion_size = param->direct_memory_onion_size;

    addr_to_check = std::bit_cast<VAddr>(param->work_memory);
    ASSERT_MSG(ValidateMappedRange(addr_to_check, param->work_memory_size, query),
               "Insufficient work memory provided");

    g_work_memory_pointer = param->work_memory;
    g_work_size = param->work_memory_size;

    // All initialization checks passed.
    LOG_INFO(Lib_VrTracker, "called, virtual headset connected = {}", HeadsetConnected());
    g_library_initialized = true;

    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceVrTrackerRegisterDevice(const OrbisVrTrackerDeviceType device_type,
                                            const s32 handle) {
    LOG_TRACE(Lib_VrTracker, "redirected to sceVrTrackerRegisterDeviceInternal");
    return sceVrTrackerRegisterDeviceInternal(device_type, handle, -1, 0);
}

s32 PS4_SYSV_ABI sceVrTrackerRegisterDevice2(const OrbisVrTrackerDeviceType device_type,
                                             const s32 handle) {
    LOG_TRACE(Lib_VrTracker, "redirected to sceVrTrackerRegisterDeviceInternal");
    return sceVrTrackerRegisterDeviceInternal(device_type, handle, -1, 1);
}

s32 PS4_SYSV_ABI sceVrTrackerRegisterDeviceInternal(const OrbisVrTrackerDeviceType device_type,
                                                    const s32 handle, s32 unk0, s32 unk1) {
    LOG_WARNING(Lib_VrTracker, "(STUBBED) called, device_type = {}, handle = {}",
                static_cast<u32>(device_type), handle);
    if (!g_library_initialized) {
        return ORBIS_VR_TRACKER_ERROR_NOT_INIT;
    }
    if (device_type > OrbisVrTrackerDeviceType::ORBIS_VR_TRACKER_DEVICE_GUN || unk0 > 4) {
        return ORBIS_VR_TRACKER_ERROR_ARGUMENT_INVALID;
    }

    // Ignore handle handle validation for now, since most of that logic isn't really handled.
    switch (device_type) {
    case OrbisVrTrackerDeviceType::ORBIS_VR_TRACKER_DEVICE_HMD: {
        if (g_hmd_handle != -1) {
            return ORBIS_VR_TRACKER_ERROR_DEVICE_ALREADY_REGISTERED;
        }
        g_hmd_handle = handle;
        break;
    }
    case OrbisVrTrackerDeviceType::ORBIS_VR_TRACKER_DEVICE_DUALSHOCK4: {
        OrbisVrTrackerLedColor color;
        bool players;
        {
            std::scoped_lock lock{g_pads_mutex};
            for (const PadRegistration& pad : g_pads) {
                if (pad.handle == handle) {
                    return ORBIS_VR_TRACKER_ERROR_DEVICE_ALREADY_REGISTERED;
                }
            }
            if (g_pads.size() >= 4) {
                return ORBIS_VR_TRACKER_ERROR_DEVICE_ALREADY_REGISTERED;
            }
            if (unk0 >= 0) {
                color = static_cast<OrbisVrTrackerLedColor>(unk0);
            } else {
                // The first colour no other controller has.
                s32 free = 0;
                while (std::ranges::any_of(g_pads, [&](const PadRegistration& pad) {
                    return static_cast<s32>(pad.color) == free;
                })) {
                    ++free;
                }
                color = static_cast<OrbisVrTrackerLedColor>(free);
            }
            g_pads.push_back({handle, color});
            players = g_pads.size() == 1;
        }
        if (!players) {
            LOG_INFO(Lib_VrTracker,
                     "Controller {} registered as well: it is not the player's, and nothing "
                     "tracks it",
                     handle);
            break;
        }
        // The camera tells controllers apart by the colour of their light bar, so the system
        // sets it, and titles refer to it ("move the red light bar..."). The real controller
        // should show the same colour.
        static constexpr u8 Colours[5][3] = {
            {0, 0, 255}, {255, 0, 0}, {0, 255, 0}, {255, 0, 255}, {255, 255, 0}};
        const auto& colour = Colours[std::clamp<s32>(static_cast<s32>(color), 0, 4)];
        Core::Vr::Runtime::Instance().SetPadLight(colour[0], colour[1], colour[2]);
        break;
    }
    case OrbisVrTrackerDeviceType::ORBIS_VR_TRACKER_DEVICE_MOVE: {
        std::scoped_lock lock{g_moves_mutex};
        if (g_moves.contains(handle)) {
            return ORBIS_VR_TRACKER_ERROR_DEVICE_ALREADY_REGISTERED;
        }
        const auto hand = Libraries::Move::HandForHandle(handle);
        if (hand < 0) return ORBIS_VR_TRACKER_ERROR_ARGUMENT_INVALID;
        g_moves.emplace(handle, hand);
        break;
    }
    case OrbisVrTrackerDeviceType::ORBIS_VR_TRACKER_DEVICE_GUN: {
        if (g_gun_handle != -1) {
            return ORBIS_VR_TRACKER_ERROR_DEVICE_ALREADY_REGISTERED;
        }
        g_gun_handle = handle;
        break;
    }
    default: {
        // Shouldn't be possible to hit this.
        UNREACHABLE();
    }
    }

    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceVrTrackerCpuProcess(const OrbisVrTrackerCpuProcessParam* param) {
    LOG_TRACE(Lib_VrTracker, "called");
    if (!g_library_initialized) {
        return ORBIS_VR_TRACKER_ERROR_NOT_INIT;
    }
    // Tracking is done by the host, there is no camera image to process.
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceVrTrackerGetPlayAreaWarningInfo(OrbisVrTrackerPlayAreaWarningInfo* info) {
    LOG_ERROR(Lib_VrTracker, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceVrTrackerGetResult(const OrbisVrTrackerGetResultParam* param,
                                       OrbisVrTrackerResultData* result) {
    LOG_TRACE(Lib_VrTracker, "called");
    if (!g_library_initialized) {
        static std::atomic<bool> warned{};
        if (!warned.exchange(true)) LOG_ERROR(Lib_VrTracker, "Tracking unavailable: tracker initialization did not succeed");
        return ORBIS_VR_TRACKER_ERROR_NOT_INIT;
    }
    if (param == nullptr || result == nullptr ||
        param->size != sizeof(OrbisVrTrackerGetResultParam) || param->handle < 0) {
        return ORBIS_VR_TRACKER_ERROR_ARGUMENT_INVALID;
    }

    const s32 handle = param->handle;
    const bool is_hmd = handle == g_hmd_handle;
    const bool is_gun = handle == g_gun_handle;
    const auto pad_registration = FindPad(handle);
    // (Another player's controller is registered, but nothing tracks it.)
    const bool is_pad = pad_registration.has_value() && IsPlayersPad(handle);
    const auto move_hand = FindMove(handle);
    if (!is_hmd && !pad_registration && !move_hand && handle != g_gun_handle) {
        return ORBIS_VR_TRACKER_ERROR_DEVICE_NOT_REGISTERED;
    }

    std::memset(result, 0, sizeof(OrbisVrTrackerResultData));
    result->handle = handle;
    result->user_frame_number = param->user_frame_number;
    result->camera_orientation_w = 1.0f;

    // The host tracks the headset, the pad and optionally two independent Move devices.
    if (!HeadsetConnected() || (!is_hmd && !is_pad && !move_hand && !is_gun)) {
        result->status = OrbisVrTrackerStatus::ORBIS_VR_TRACKER_STATUS_NOT_TRACKING;
        return ORBIS_OK;
    }

    auto& runtime = Core::Vr::Runtime::Instance();
    const auto move = move_hand ? runtime.GetMove(*move_hand) : Core::Vr::MoveState{};
    if (move_hand && (!move.connected || !move.device.tracked)) {
        result->connected = move.connected;
        result->status = OrbisVrTrackerStatus::ORBIS_VR_TRACKER_STATUS_NOT_TRACKING;
        return ORBIS_OK;
    }
    const Core::Vr::DeviceState state = is_gun ? runtime.GetAim() : move_hand ? move.device
        : (is_hmd ? runtime.GetHead() : runtime.GetPad());
    if (is_gun && (!runtime.AimEnabled() || !state.tracked)) {
        result->connected = runtime.AimEnabled();
        result->status = OrbisVrTrackerStatus::ORBIS_VR_TRACKER_STATUS_NOT_TRACKING;
        return ORBIS_OK;
    }

    static std::atomic<u32> result_calls{};
    if (const u32 call = result_calls.fetch_add(1); call < 4 || call % 1200 == 0) {
        LOG_DEBUG(Lib_VrTracker,
                  "call {}: handle = {:#x}, result_type = {}, prediction_time = {}, orientation_type "
                  "= {}, usage_type = {}, position = ({:.3f}, {:.3f}, {:.3f})",
                  call, handle, static_cast<u32>(param->result_type), param->prediction_time,
                  static_cast<u32>(param->orientation_type), static_cast<u32>(param->usage_type),
                  state.pose.position.x, state.pose.position.y, state.pose.position.z);
    }

    // The host already predicts poses for the moment its display lights up, which replaces the
    // prediction the guest asks for here.
    const u64 now = Libraries::Kernel::sceKernelGetProcessTime();
    result->connected = 1;
    result->timestamp = param->prediction_time != 0 ? param->prediction_time : now;
    result->device_timestamp = now;
    result->recalibrate_necessity =
        OrbisVrTrackerRecalibrateNecessityType::ORBIS_VR_TRACKER_RECALIBRATE_NECESSITY_NOTHING;
    result->playarea_brightness_risk =
        OrbisVrTrackerPlayareaBrightnessRiskType::ORBIS_VR_TRACKER_PLAYAREA_BRIGHTNESS_RISK_LOW;
    result->led_color = is_pad ? pad_registration->color
                               : OrbisVrTrackerLedColor::ORBIS_VR_TRACKER_LED_COLOR_BLUE;
    result->status = OrbisVrTrackerStatus::ORBIS_VR_TRACKER_STATUS_TRACKING;
    {
        std::scoped_lock lock{g_calibration_mutex};
        if (auto pending = g_calibrations.find(handle); pending != g_calibrations.end()) {
            if (pending->second.IsCalibrating(CalibrationState::Clock::now())) {
                result->status = OrbisVrTrackerStatus::ORBIS_VR_TRACKER_STATUS_CALIBRATING;
            } else {
                g_calibrations.erase(pending);
                LOG_INFO(Lib_VrTracker, "Virtual calibration complete for handle {:#x}", handle);
            }
        }
    }
    result->position_quality = OrbisVrTrackerQuality::ORBIS_VR_TRACKER_QUALITY_FULL;
    result->orientation_quality = OrbisVrTrackerQuality::ORBIS_VR_TRACKER_QUALITY_FULL;
    result->velocity_x = state.linear_velocity.x;
    result->velocity_y = state.linear_velocity.y;
    result->velocity_z = state.linear_velocity.z;
    result->angular_velocity_x = state.angular_velocity.x;
    result->angular_velocity_y = state.angular_velocity.y;
    result->angular_velocity_z = state.angular_velocity.z;

    if (is_pad) {
        WritePose(result->pad_info.device_pose, state.pose);
        if (std::getenv("SHADPS4_TRACE_HMD_LAYOUT")) {
            static std::atomic<u32> pad_calls{};
            if (pad_calls.fetch_add(1) % 300 == 0) {
                LOG_INFO(Lib_VrTracker, "Guest pad tracking handle={} type={} tracked={} "
                         "status={} position={}/{}/{} orientation={}/{}/{}/{}",
                         handle, static_cast<u32>(param->result_type), state.tracked,
                         static_cast<u32>(result->status), state.pose.position.x,
                         state.pose.position.y, state.pose.position.z, state.pose.orientation.x,
                         state.pose.orientation.y, state.pose.orientation.z, state.pose.orientation.w);
            }
        }
        return ORBIS_OK;
    }
    if (move_hand) {
        WritePose(result->move_info.device_pose, state.pose);
        return ORBIS_OK;
    }
    if (is_gun) {
        WritePose(result->gun_info.device_pose, state.pose);
        static const bool trace = std::getenv("SHADPS4_TRACE_HMD_LAYOUT") != nullptr;
        static std::atomic<u32> gun_calls{};
        if (trace && gun_calls.fetch_add(1) % 300 == 0) {
            LOG_INFO(Lib_VrTracker, "Guest Aim tracking handle={} status={} position={}/{}/{} orientation={}/{}/{}/{}",
                     handle, static_cast<u32>(result->status), state.pose.position.x,
                     state.pose.position.y, state.pose.position.z, state.pose.orientation.x,
                     state.pose.orientation.y, state.pose.orientation.z, state.pose.orientation.w);
        }
        return ORBIS_OK;
    }

    auto& hmd = result->hmd_info;
    WritePose(hmd.device_pose, state.pose);
    WritePose(hmd.head_pose, state.pose);
    // The eyes sit half the interpupillary distance either side of the headset centre.
    const float half_ipd = runtime.GetConfig().ipd * 0.5f;
    const Core::Vr::Vec3 right = Core::Vr::Rotate(state.pose.orientation, {half_ipd, 0.0f, 0.0f});
    Core::Vr::Pose eye = state.pose;
    eye.position = {state.pose.position.x - right.x, state.pose.position.y - right.y,
                    state.pose.position.z - right.z};
    WritePose(hmd.left_eye_pose, eye);
    eye.position = {state.pose.position.x + right.x, state.pose.position.y + right.y,
                    state.pose.position.z + right.z};
    WritePose(hmd.right_eye_pose, eye);
    hmd.rear_tracking_status =
        OrbisVrTrackerHmdRearTrackingStatus::ORBIS_VR_TRACKER_REAR_TRACKING_READY;
    hmd.sensor_read_system_timestamp = now;

    Core::KnownTitle::NoteView(state.pose.position);
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceVrTrackerGetTime(u64* time) {
    LOG_TRACE(Lib_VrTracker, "called");
    if (!g_library_initialized) {
        return ORBIS_VR_TRACKER_ERROR_NOT_INIT;
    }
    if (time == nullptr) {
        return ORBIS_VR_TRACKER_ERROR_ARGUMENT_INVALID;
    }
    *time = Libraries::Kernel::sceKernelGetProcessTime();
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceVrTrackerGpuSubmit(const OrbisVrTrackerGpuSubmitParam* param) {
    LOG_TRACE(Lib_VrTracker, "called");
    if (!g_library_initialized) {
        return ORBIS_VR_TRACKER_ERROR_NOT_INIT;
    }
    if (!HeadsetConnected()) {
        // Impossible to submit valid data here since sceCameraGetFrameData returns an error.
        return ORBIS_VR_TRACKER_ERROR_ARGUMENT_INVALID;
    }
    // SDK 3.50 (Rush of Blood CUSA02350 01.00) submits a 0x20-byte header
    // followed by the older 0x208-byte camera frame. The newer layout has a
    // 0x40-byte header and a 0x248-byte frame. Only the size is read here:
    // tracking uses host poses, so neither guest camera layout needs decoding.
    static_assert(sizeof(OrbisVrTrackerGpuSubmitParam) == 0x288);
    if (param == nullptr ||
        (param->size != sizeof(OrbisVrTrackerGpuSubmitParam) && param->size != 0x228)) {
        return ORBIS_VR_TRACKER_ERROR_ARGUMENT_INVALID;
    }

    // The camera frame would be analysed on the GPU here. The host tracks the devices instead.
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceVrTrackerGpuWait(const OrbisVrTrackerGpuWaitParam* param) {
    LOG_TRACE(Lib_VrTracker, "called");
    if (!g_library_initialized) {
        return ORBIS_VR_TRACKER_ERROR_NOT_INIT;
    }
    if (param == nullptr || param->size != sizeof(OrbisVrTrackerGpuWaitParam)) {
        return ORBIS_VR_TRACKER_ERROR_ARGUMENT_INVALID;
    }
    if (HeadsetConnected()) {
        return ORBIS_OK;
    }

    // Impossible to perform GPU submits
    return ORBIS_VR_TRACKER_ERROR_NOT_EXECUTE_GPU_SUBMIT;
}

s32 PS4_SYSV_ABI sceVrTrackerGpuWaitAndCpuProcess() {
    LOG_ERROR(Lib_VrTracker, "(STUBBED) called");
    if (!g_library_initialized) {
        return ORBIS_VR_TRACKER_ERROR_NOT_INIT;
    }
    if (HeadsetConnected()) {
        return ORBIS_OK;
    }

    // Impossible to perform GPU submits
    return ORBIS_VR_TRACKER_ERROR_NOT_EXECUTE_GPU_SUBMIT;
}

s32 PS4_SYSV_ABI
sceVrTrackerNotifyEndOfCpuProcess(const OrbisVrTrackerNotifyEndOfCpuProcessParam* param) {
    LOG_ERROR(Lib_VrTracker, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceVrTrackerRecalibrate(const OrbisVrTrackerRecalibrateParam* param) {
    LOG_TRACE(Lib_VrTracker, "called");
    if (!g_library_initialized) {
        return ORBIS_VR_TRACKER_ERROR_NOT_INIT;
    }
    if (param == nullptr || param->size != sizeof(OrbisVrTrackerRecalibrateParam) ||
        param->device_type > OrbisVrTrackerDeviceType::ORBIS_VR_TRACKER_DEVICE_GUN) {
        return ORBIS_VR_TRACKER_ERROR_ARGUMENT_INVALID;
    }

    std::vector<s32> handles;
    OrbisVrTrackerDeviceType device_type = param->device_type;
    switch (device_type) {
    case OrbisVrTrackerDeviceType::ORBIS_VR_TRACKER_DEVICE_HMD: {
        // Seems like the lack of a connected hmd results in this?
        if (!HeadsetConnected() || g_hmd_handle == -1) {
            return ORBIS_VR_TRACKER_ERROR_DEVICE_NOT_REGISTERED;
        }
        handles.push_back(g_hmd_handle);
        break;
    }
    case OrbisVrTrackerDeviceType::ORBIS_VR_TRACKER_DEVICE_DUALSHOCK4: {
        std::scoped_lock lock{g_pads_mutex};
        if (g_pads.empty()) {
            return ORBIS_VR_TRACKER_ERROR_DEVICE_NOT_REGISTERED;
        }
        for (const auto& pad : g_pads) {
            handles.push_back(pad.handle);
        }
        break;
    }
    case OrbisVrTrackerDeviceType::ORBIS_VR_TRACKER_DEVICE_MOVE: {
        std::scoped_lock lock{g_moves_mutex};
        if (g_moves.empty()) {
            return ORBIS_VR_TRACKER_ERROR_DEVICE_NOT_REGISTERED;
        }
        for (const auto& [handle, hand] : g_moves) handles.push_back(handle);
        break;
    }
    case OrbisVrTrackerDeviceType::ORBIS_VR_TRACKER_DEVICE_GUN: {
        if (g_gun_handle == -1) {
            return ORBIS_VR_TRACKER_ERROR_DEVICE_NOT_REGISTERED;
        }
        handles.push_back(g_gun_handle);
        break;
    }
    default: {
        // Shouldn't be possible to hit this.
        UNREACHABLE();
    }
    }

    // No camera calibration is necessary for host poses. Do report its lifecycle:
    // returning success alone leaves guests waiting for calibration completion.
    std::scoped_lock lock{g_calibration_mutex};
    for (const s32 handle : handles) {
        g_calibrations[handle].Request();
        LOG_INFO(Lib_VrTracker, "Virtual calibration requested for handle {:#x}", handle);
    }
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceVrTrackerResetAll() {
    LOG_ERROR(Lib_VrTracker, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceVrTrackerResetOrientationRelative(const OrbisVrTrackerDeviceType device_type,
                                                      const s32 handle) {
    LOG_ERROR(Lib_VrTracker, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceVrTrackerSaveInternalBuffers() {
    LOG_ERROR(Lib_VrTracker, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceVrTrackerSetDurationUntilStatusNotTracking(
    const OrbisVrTrackerDeviceType device_type, const u32 duration_camera_frames) {
    LOG_ERROR(Lib_VrTracker, "(STUBBED) called");
    if (!g_library_initialized) {
        return ORBIS_VR_TRACKER_ERROR_NOT_INIT;
    }
    if (device_type > OrbisVrTrackerDeviceType::ORBIS_VR_TRACKER_DEVICE_GUN) {
        return ORBIS_VR_TRACKER_ERROR_ARGUMENT_INVALID;
    }

    // Seems to unconditionally return 0 when parameters are valid.
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceVrTrackerSetExtendedMode() {
    LOG_ERROR(Lib_VrTracker, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceVrTrackerSetLEDBrightness() {
    LOG_ERROR(Lib_VrTracker, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceVrTrackerSetRestingMode() {
    LOG_ERROR(Lib_VrTracker, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI
sceVrTrackerUpdateMotionSensorData(const OrbisVrTrackerUpdateMotionSensorDataParam* param) {
    LOG_ERROR(Lib_VrTracker, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI Func_0FA4C949F8D3024E() {
    LOG_ERROR(Lib_VrTracker, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI Func_285C6AFC09C42F7E() {
    LOG_ERROR(Lib_VrTracker, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI Func_9A6CDB2103664F8A() {
    LOG_ERROR(Lib_VrTracker, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI Func_B4D26B7D8B18DF06(const OrbisVrTrackerDeviceType device_type, const s32 handle,
                                       const s32 led_color) {
    // Undocumented registration variant: the third argument picks the light bar colour the
    // tracker should assign to the device (0..4), instead of leaving the choice to the system.
    LOG_INFO(Lib_VrTracker, "called, device_type = {}, handle = {}, led_color = {}",
             static_cast<u32>(device_type), handle, led_color);
    return sceVrTrackerRegisterDeviceInternal(device_type, handle, led_color, 1);
}

s32 PS4_SYSV_ABI sceVrTrackerSetDeviceRejection() {
    LOG_ERROR(Lib_VrTracker, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI Func_1119B0BE399F37E7() {
    LOG_ERROR(Lib_VrTracker, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI Func_4928B43816BC440D() {
    LOG_ERROR(Lib_VrTracker, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI Func_863EF32EFCB0FA9C() {
    LOG_ERROR(Lib_VrTracker, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI Func_E6E726CBC85C48F9() {
    LOG_ERROR(Lib_VrTracker, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI Func_F6407E46C66DF383() {
    LOG_ERROR(Lib_VrTracker, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceVrTrackerCpuPopMarker() {
    LOG_ERROR(Lib_VrTracker, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceVrTrackerCpuPushMarker() {
    LOG_ERROR(Lib_VrTracker, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceVrTrackerGetLiveCaptureId() {
    LOG_ERROR(Lib_VrTracker, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceVrTrackerStartLiveCapture() {
    LOG_ERROR(Lib_VrTracker, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceVrTrackerStopLiveCapture() {
    LOG_ERROR(Lib_VrTracker, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceVrTrackerUnregisterDevice(const s32 handle) {
    LOG_DEBUG(Lib_VrTracker, "called");
    if (!g_library_initialized) {
        return ORBIS_VR_TRACKER_ERROR_NOT_INIT;
    }
    if (handle < 0) {
        return ORBIS_VR_TRACKER_ERROR_ARGUMENT_INVALID;
    }
    // Since this function only takes a handle, compare the handle to registered handles.
    if (handle == g_hmd_handle) {
        g_hmd_handle = -1;
    } else if (FindPad(handle)) {
        // (The next registered, if any, is the player's from now on.)
        std::scoped_lock lock{g_pads_mutex};
        std::erase_if(g_pads, [&](const PadRegistration& pad) { return pad.handle == handle; });
    } else if (FindMove(handle)) {
        std::scoped_lock lock{g_moves_mutex};
        g_moves.erase(handle);
    } else if (handle == g_gun_handle) {
        g_gun_handle = -1;
    } else {
        // If none of the handles match up, then return an error.
        return ORBIS_VR_TRACKER_ERROR_ARGUMENT_INVALID;
    }

    {
        std::scoped_lock lock{g_calibration_mutex};
        g_calibrations.erase(handle);
    }
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceVrTrackerTerm() {
    LOG_DEBUG(Lib_VrTracker, "called");
    if (!g_library_initialized) {
        return ORBIS_VR_TRACKER_ERROR_NOT_INIT;
    }
    g_library_initialized = false;
    {
        std::scoped_lock lock{g_moves_mutex};
        g_moves.clear();
    }
    {
        std::scoped_lock lock{g_calibration_mutex};
        g_calibrations.clear();
    }
    return ORBIS_OK;
}

void RegisterLib(Core::Loader::SymbolsResolver* sym) {
    LIB_FUNCTION("24kDA+A0Ox0", "libSceVrTrackerFourDeviceAllowed", 1, "libSceVrTracker",
                 sceVrTrackerRegisterDevice2);
    LIB_FUNCTION("5IFOAYv-62g", "libSceVrTracker", 1, "libSceVrTracker", sceVrTrackerCpuProcess);
    LIB_FUNCTION("zvyKP0Z3UvU", "libSceVrTracker", 1, "libSceVrTracker",
                 sceVrTrackerGetPlayAreaWarningInfo);
    LIB_FUNCTION("76OBvrrQXUc", "libSceVrTracker", 1, "libSceVrTracker", sceVrTrackerGetResult);
    LIB_FUNCTION("XoeWzXlrnMw", "libSceVrTracker", 1, "libSceVrTracker", sceVrTrackerGetTime);
    LIB_FUNCTION("TVegDMLaBB8", "libSceVrTracker", 1, "libSceVrTracker", sceVrTrackerGpuSubmit);
    LIB_FUNCTION("gkGuO9dd57M", "libSceVrTracker", 1, "libSceVrTracker", sceVrTrackerGpuWait);
    LIB_FUNCTION("ARhgpXvwoR0", "libSceVrTracker", 1, "libSceVrTracker",
                 sceVrTrackerGpuWaitAndCpuProcess);
    LIB_FUNCTION("QkRl7pART9M", "libSceVrTracker", 1, "libSceVrTracker", sceVrTrackerInit);
    LIB_FUNCTION("VItTwN8DmS8", "libSceVrTracker", 1, "libSceVrTracker",
                 sceVrTrackerNotifyEndOfCpuProcess);
    LIB_FUNCTION("K7yhYrsIBPc", "libSceVrTracker", 1, "libSceVrTracker", sceVrTrackerQueryMemory);
    LIB_FUNCTION("EUCaQtXXYNI", "libSceVrTracker", 1, "libSceVrTracker", sceVrTrackerRecalibrate);
    LIB_FUNCTION("sIh8GwcevaQ", "libSceVrTracker", 1, "libSceVrTracker",
                 sceVrTrackerRegisterDevice);
    LIB_FUNCTION("ufexf4aNiwg", "libSceVrTracker", 1, "libSceVrTracker",
                 sceVrTrackerRegisterDeviceInternal);
    LIB_FUNCTION("CtWUbFgmq+I", "libSceVrTracker", 1, "libSceVrTracker", sceVrTrackerResetAll);
    LIB_FUNCTION("E0P0sN-wy+4", "libSceVrTracker", 1, "libSceVrTracker",
                 sceVrTrackerResetOrientationRelative);
    LIB_FUNCTION("bDGZVTwwZ1A", "libSceVrTracker", 1, "libSceVrTracker",
                 sceVrTrackerSaveInternalBuffers);
    LIB_FUNCTION("qBjnR0HtMYI", "libSceVrTracker", 1, "libSceVrTracker",
                 sceVrTrackerSetDurationUntilStatusNotTracking);
    LIB_FUNCTION("NhPkY3V8E+8", "libSceVrTracker", 1, "libSceVrTracker",
                 sceVrTrackerSetExtendedMode);
    LIB_FUNCTION("vpsLLotiSUg", "libSceVrTracker", 1, "libSceVrTracker",
                 sceVrTrackerSetLEDBrightness);
    LIB_FUNCTION("lgWSHQ8p4i4", "libSceVrTracker", 1, "libSceVrTracker",
                 sceVrTrackerSetRestingMode);
    LIB_FUNCTION("IBv4P3q1pQ0", "libSceVrTracker", 1, "libSceVrTracker", sceVrTrackerTerm);
    LIB_FUNCTION("Q8skQqEwn5c", "libSceVrTracker", 1, "libSceVrTracker",
                 sceVrTrackerUnregisterDevice);
    LIB_FUNCTION("9fvHMUbsom4", "libSceVrTracker", 1, "libSceVrTracker",
                 sceVrTrackerUpdateMotionSensorData);
    LIB_FUNCTION("D6TJSfjTAk4", "libSceVrTracker", 1, "libSceVrTracker", Func_0FA4C949F8D3024E);
    LIB_FUNCTION("KFxq-AnEL34", "libSceVrTracker", 1, "libSceVrTracker", Func_285C6AFC09C42F7E);
    LIB_FUNCTION("mmzbIQNmT4o", "libSceVrTracker", 1, "libSceVrTracker", Func_9A6CDB2103664F8A);
    LIB_FUNCTION("tNJrfYsY3wY", "libSceVrTracker", 1, "libSceVrTracker", Func_B4D26B7D8B18DF06);
    LIB_FUNCTION("jGqEkPy0iLU", "libSceVrTrackerDeviceRejection", 1, "libSceVrTracker",
                 sceVrTrackerSetDeviceRejection);
    LIB_FUNCTION("ERmwvjmfN+c", "libSceVrTrackerGpuTest", 1, "libSceVrTracker",
                 Func_1119B0BE399F37E7);
    LIB_FUNCTION("SSi0OBa8RA0", "libSceVrTrackerGpuTest", 1, "libSceVrTracker",
                 Func_4928B43816BC440D);
    LIB_FUNCTION("hj7zLvyw+pw", "libSceVrTrackerGpuTest", 1, "libSceVrTracker",
                 Func_863EF32EFCB0FA9C);
    LIB_FUNCTION("5ucmy8hcSPk", "libSceVrTrackerGpuTest", 1, "libSceVrTracker",
                 Func_E6E726CBC85C48F9);
    LIB_FUNCTION("9kB+RsZt84M", "libSceVrTrackerGpuTest", 1, "libSceVrTracker",
                 Func_F6407E46C66DF383);
    LIB_FUNCTION("sBkAqyF5Gns", "libSceVrTrackerLiveCapture", 1, "libSceVrTracker",
                 sceVrTrackerCpuPopMarker);
    LIB_FUNCTION("rvCywCbc7Pk", "libSceVrTrackerLiveCapture", 1, "libSceVrTracker",
                 sceVrTrackerCpuPushMarker);
    LIB_FUNCTION("lm6T1Ur6JRk", "libSceVrTrackerLiveCapture", 1, "libSceVrTracker",
                 sceVrTrackerGetLiveCaptureId);
    LIB_FUNCTION("qa1+CeXKDPc", "libSceVrTrackerLiveCapture", 1, "libSceVrTracker",
                 sceVrTrackerStartLiveCapture);
    LIB_FUNCTION("3YCwwpHkHIg", "libSceVrTrackerLiveCapture", 1, "libSceVrTracker",
                 sceVrTrackerStopLiveCapture);
};

} // namespace Libraries::VrTracker
