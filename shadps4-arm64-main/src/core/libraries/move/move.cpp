// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "common/logging/log.h"
#include "core/libraries/error_codes.h"
#include "core/libraries/libs.h"
#include "core/libraries/move/move.h"
#include "core/libraries/move/move_error.h"
#include <array>
#include <algorithm>
#include <mutex>
#include <unordered_map>
#include "core/libraries/kernel/time.h"
#include "core/vr/vr_runtime.h"

namespace Libraries::Move {

static std::atomic<bool> g_library_initialized = false;
static std::mutex g_mutex;
struct Registration {
    s32 user; s32 type; s32 index; s32 hand;
    u64 sample_sequence{};
    s64 sample_timestamp{};
};
static std::unordered_map<s32, Registration> g_handles;
static s32 g_next_handle = 0x30b0000;

s32 HandForHandle(s32 handle) {
    std::scoped_lock lock{g_mutex};
    const auto it = g_handles.find(handle);
    return it == g_handles.end() ? -1 : it->second.hand;
}

static s32 ReadMove(s32 handle, OrbisMoveData* data) {
    if (!g_library_initialized) return ORBIS_MOVE_ERROR_NOT_INIT;
    if (!data) return ORBIS_MOVE_ERROR_INVALID_ARG;
    *data = {};
    const s32 hand = HandForHandle(handle);
    if (hand < 0) return ORBIS_MOVE_ERROR_INVALID_HANDLE;
    const auto state = Core::Vr::Runtime::Instance().GetMove(hand);
    if (!state.connected) return ORBIS_MOVE_ERROR_NO_CONTROLLER_CONNECTED;
    const auto gravity = Core::Vr::Rotate(Core::Vr::Conjugate(state.device.pose.orientation), {0, 1, 0});
    data->accelerometer[0] = gravity.x;
    data->accelerometer[1] = gravity.y;
    data->accelerometer[2] = gravity.z;
    const auto gyro = Core::Vr::Rotate(Core::Vr::Conjugate(state.device.pose.orientation), state.device.angular_velocity);
    data->gyro[0] = gyro.x;
    data->gyro[1] = gyro.y;
    data->gyro[2] = gyro.z;
    data->button_data = {state.buttons, state.trigger};
    const auto age = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - state.sampled).count();
    {
        std::scoped_lock lock{g_mutex};
        const auto found = g_handles.find(handle);
        if (found == g_handles.end()) return ORBIS_MOVE_ERROR_INVALID_HANDLE;
        auto& registration = found->second;
        if (registration.sample_sequence != state.device.sequence) {
            registration.sample_sequence = state.device.sequence;
            registration.sample_timestamp = std::max<s64>(registration.sample_timestamp + 1,
                static_cast<s64>(Kernel::sceKernelGetProcessTime()) - age);
        }
        data->timestamp = registration.sample_timestamp;
    }
    data->count = static_cast<s32>(state.device.sequence & 0x7fffffff);
    data->temperature = 25.0f;
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceMoveInit() {
    if (g_library_initialized) {
        return ORBIS_MOVE_ERROR_ALREADY_INIT;
    }
    LOG_WARNING(Lib_Move, "Experimental two-controller Move bridge enabled = {}",
                Core::Vr::Runtime::Instance().MovesEnabled());
    g_library_initialized = true;
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceMoveOpen(Libraries::UserService::OrbisUserServiceUserId user_id, s32 type,
                             s32 index) {
    LOG_DEBUG(Lib_Move, "called");
    if (!g_library_initialized) {
        return ORBIS_MOVE_ERROR_NOT_INIT;
    }
    // Even when no controllers are connected, this returns a proper handle.
    // Internal libSceVrTracker logic requires this handle to be different from other devices.
    std::scoped_lock lock{g_mutex};
    if (index < 0 || index > 1) return ORBIS_MOVE_ERROR_INVALID_PORT;
    for (const auto& [handle, registration] : g_handles) {
        if (registration.user == user_id && registration.type == type && registration.index == index)
            return ORBIS_MOVE_ERROR_ALREADY_OPENED;
    }
    std::array<bool, 2> used{};
    for (const auto& [handle, registration] : g_handles) used[registration.hand] = true;
    const s32 hand = !used[index] ? index : (!used[0] ? 0 : (!used[1] ? 1 : -1));
    if (hand < 0) return ORBIS_MOVE_ERROR_MAX_CONTROLLERS_EXCEEDED;
    const s32 handle = g_next_handle += 0x100;
    g_handles.emplace(handle, Registration{user_id, type, index, hand});
    LOG_INFO(Lib_Move, "Opened Move handle {:#x}: user={} type={} index={} hand={}",
             handle, user_id, type, index, hand);
    return handle;
}

s32 PS4_SYSV_ABI sceMoveGetDeviceInfo(s32 handle, OrbisMoveDeviceInfo* info) {
    LOG_TRACE(Lib_Move, "called");
    if (!g_library_initialized) {
        return ORBIS_MOVE_ERROR_NOT_INIT;
    }
    if (info == nullptr) {
        return ORBIS_MOVE_ERROR_INVALID_ARG;
    }
    *info = {};
    const auto hand = HandForHandle(handle);
    if (hand < 0) return ORBIS_MOVE_ERROR_INVALID_HANDLE;
    if (!Core::Vr::Runtime::Instance().GetMove(hand).connected)
        return ORBIS_MOVE_ERROR_NO_CONTROLLER_CONNECTED;
    info->sphere_radius = 0.0225f;
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceMoveReadStateLatest(s32 handle, OrbisMoveData* data) {
    LOG_TRACE(Lib_Move, "called");
    if (!g_library_initialized) {
        return ORBIS_MOVE_ERROR_NOT_INIT;
    }
    if (data == nullptr) {
        return ORBIS_MOVE_ERROR_INVALID_ARG;
    }
    return ReadMove(handle, data);
}

s32 PS4_SYSV_ABI sceMoveReadStateRecent(s32 handle, s64 timestamp, OrbisMoveData* data,
                                        s32* out_count) {
    LOG_TRACE(Lib_Move, "called");
    if (!g_library_initialized) {
        return ORBIS_MOVE_ERROR_NOT_INIT;
    }
    if (timestamp < 0 || data == nullptr || out_count == nullptr) {
        return ORBIS_MOVE_ERROR_INVALID_ARG;
    }
    *out_count = 0;
    OrbisMoveData latest{};
    const auto result = ReadMove(handle, &latest);
    if (result != ORBIS_OK) return result;
    // Report the latest available host sample once; do not invent older samples.
    if (latest.timestamp > timestamp) {
        *data = latest;
        *out_count = 1;
    }
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceMoveGetExtensionPortInfo(s32 handle, void* data) {
    LOG_TRACE(Lib_Move, "called");
    if (!g_library_initialized) {
        return ORBIS_MOVE_ERROR_NOT_INIT;
    }
    if (data == nullptr) {
        return ORBIS_MOVE_ERROR_INVALID_ARG;
    }
    if (HandForHandle(handle) < 0) return ORBIS_MOVE_ERROR_INVALID_HANDLE;
    return ORBIS_MOVE_ERROR_NO_CONTROLLER_CONNECTED;
}

s32 PS4_SYSV_ABI sceMoveSetVibration(s32 handle, u8 intensity) {
    LOG_TRACE(Lib_Move, "called");
    if (!g_library_initialized) {
        return ORBIS_MOVE_ERROR_NOT_INIT;
    }
    const auto hand = HandForHandle(handle);
    if (hand < 0) return ORBIS_MOVE_ERROR_INVALID_HANDLE;
    // Haptics are not yet forwarded in this experimental bridge.
    return Core::Vr::Runtime::Instance().GetMove(hand).connected ? ORBIS_OK
        : ORBIS_MOVE_ERROR_NO_CONTROLLER_CONNECTED;
}

s32 PS4_SYSV_ABI sceMoveSetLightSphere(s32 handle, u8 red, u8 green, u8 blue) {
    LOG_TRACE(Lib_Move, "called");
    if (!g_library_initialized) {
        return ORBIS_MOVE_ERROR_NOT_INIT;
    }
    const auto hand = HandForHandle(handle);
    if (hand < 0) return ORBIS_MOVE_ERROR_INVALID_HANDLE;
    return Core::Vr::Runtime::Instance().GetMove(hand).connected ? ORBIS_OK
        : ORBIS_MOVE_ERROR_NO_CONTROLLER_CONNECTED;
}

s32 PS4_SYSV_ABI sceMoveResetLightSphere(s32 handle) {
    LOG_TRACE(Lib_Move, "called");
    if (!g_library_initialized) {
        return ORBIS_MOVE_ERROR_NOT_INIT;
    }
    if (HandForHandle(handle) < 0) return ORBIS_MOVE_ERROR_INVALID_HANDLE;
    // Returns success for an open handle, even if no controllers are connected.
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceMoveClose(s32 handle) {
    LOG_DEBUG(Lib_Move, "called");
    if (!g_library_initialized) {
        return ORBIS_MOVE_ERROR_NOT_INIT;
    }
    std::scoped_lock lock{g_mutex};
    return g_handles.erase(handle) ? ORBIS_OK : ORBIS_MOVE_ERROR_INVALID_HANDLE;
}

s32 PS4_SYSV_ABI sceMoveTerm() {
    LOG_DEBUG(Lib_Move, "called");
    if (!g_library_initialized) {
        return ORBIS_MOVE_ERROR_NOT_INIT;
    }
    g_library_initialized = false;
    std::scoped_lock lock{g_mutex};
    g_handles.clear();
    return ORBIS_OK;
}

void RegisterLib(Core::Loader::SymbolsResolver* sym) {
    LIB_FUNCTION("j1ITE-EoJmE", "libSceMove", 1, "libSceMove", sceMoveInit);
    LIB_FUNCTION("HzC60MfjJxU", "libSceMove", 1, "libSceMove", sceMoveOpen);
    LIB_FUNCTION("GWXTyxs4QbE", "libSceMove", 1, "libSceMove", sceMoveGetDeviceInfo);
    LIB_FUNCTION("ttU+JOhShl4", "libSceMove", 1, "libSceMove", sceMoveReadStateLatest);
    LIB_FUNCTION("f2bcpK6kJfg", "libSceMove", 1, "libSceMove", sceMoveReadStateRecent);
    LIB_FUNCTION("y5h7f8H1Jnk", "libSceMove", 1, "libSceMove", sceMoveGetExtensionPortInfo);
    LIB_FUNCTION("IFQwtT2CeY0", "libSceMove", 1, "libSceMove", sceMoveSetVibration);
    LIB_FUNCTION("T8KYHPs1JE8", "libSceMove", 1, "libSceMove", sceMoveSetLightSphere);
    LIB_FUNCTION("zuxWAg3HAac", "libSceMove", 1, "libSceMove", sceMoveResetLightSphere);
    LIB_FUNCTION("XX6wlxpHyeo", "libSceMove", 1, "libSceMove", sceMoveClose);
    LIB_FUNCTION("tsZi60H4ypY", "libSceMove", 1, "libSceMove", sceMoveTerm);
};

} // namespace Libraries::Move
