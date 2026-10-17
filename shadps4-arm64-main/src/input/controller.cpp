// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <sstream>
#include <unordered_set>
#include <SDL3/SDL.h>
#include <common/elf_info.h>
#include <common/singleton.h>
#include "common/singleton.h"
#include "core/vr/vr_runtime.h"
#include "common/logging/log.h"
#include "common/elf_info.h"
#include "controller.h"
#include "core/emulator_settings.h"
#include "core/libraries/kernel/time.h"
#include "core/libraries/pad/pad.h"
#include "core/libraries/system/userservice.h"
#include "core/user_settings.h"
#include "input/controller.h"

namespace Input {

using Libraries::Pad::OrbisPadButtonDataOffset;

void State::OnButton(OrbisPadButtonDataOffset button, bool isPressed) {
    if (isPressed) {
        buttonsState |= button;
    } else {
        buttonsState &= ~button;
    }
}

void State::OnAxis(Axis axis, int value, bool smooth) {
    auto const i = std::to_underlying(axis);
    // forcibly finish the previous smoothing task by jumping to the end
    axes[i] = axis_smoothing_end_values[i];

    axis_smoothing_start_times[i] = time;
    axis_smoothing_start_values[i] = axes[i];
    axis_smoothing_end_values[i] = value;
    axis_smoothing_flags[i] = smooth;
    const auto toggle = [&](const auto button) {
        if (value > 0) {
            buttonsState |= button;
        } else {
            buttonsState &= ~button;
        }
    };
    switch (axis) {
    case Axis::TriggerLeft:
        toggle(OrbisPadButtonDataOffset::L2);
        break;
    case Axis::TriggerRight:
        toggle(OrbisPadButtonDataOffset::R2);
        break;
    default:
        break;
    }
}

void State::OnTouchpad(int touchIndex, bool isDown, float x, float y) {
    touchpad[touchIndex].state = isDown;
    touchpad[touchIndex].x = static_cast<u16>(x * 1920);
    touchpad[touchIndex].y = static_cast<u16>(y * 941);
}

void State::OnGyro(const float gyro[3]) {
    angularVelocity.x = gyro[0];
    angularVelocity.y = gyro[1];
    angularVelocity.z = gyro[2];
}

void State::OnAccel(const float accel[3]) {
    acceleration.x = accel[0];
    acceleration.y = accel[1];
    acceleration.z = accel[2];
}

void State::UpdateAxisSmoothing() {
    for (int i = 0; i < std::to_underlying(Axis::AxisMax); i++) {
        // if it's not to be smoothed or close enough, just jump to the end
        if (!axis_smoothing_flags[i] || std::abs(axes[i] - axis_smoothing_end_values[i]) < 16) {
            if (axes[i] != axis_smoothing_end_values[i]) {
                axes[i] = axis_smoothing_end_values[i];
            }
            continue;
        }
        auto now = Libraries::Kernel::sceKernelGetProcessTime();
        f32 t =
            std::clamp((now - axis_smoothing_start_times[i]) / f32{axis_smoothing_time}, 0.f, 1.f);
        axes[i] = s32(axis_smoothing_start_values[i] * (1 - t) + axis_smoothing_end_values[i] * t);
    }
}

GameController::GameController() : m_states_queue(64) {}

void GameController::ReadState(State* state, bool* isConnected, int* connectedCount) {
    *isConnected = m_connected;
    *connectedCount = m_connected_count;
    *state = m_state;
}

int GameController::ReadStates(State* states, int states_num, bool* isConnected,
                               int* connectedCount) {
    *isConnected = m_connected;
    *connectedCount = m_connected_count;

    int ret_num = 0;
    if (m_connected) {
        std::lock_guard lg(m_states_queue_mutex);
        for (int i = 0; i < states_num; i++) {
            auto o_state = m_states_queue.Pop();
            if (!o_state) {
                break;
            }
            states[ret_num++] = *o_state;
        }
    }
    return ret_num;
}

/// A VR host drives the real controller of the first player.
static bool IsFirstController(const GameController* controller) {
    return (*Common::Singleton<GameControllers>::Instance())[0] == controller;
}

/// Some buttons of the first player's controller also mean something about the view in a
/// headset (see Core::Vr::Runtime::NotePadButton).
static void NoteViewButtons(OrbisPadButtonDataOffset before, OrbisPadButtonDataOffset after) {
    using Core::Vr::Runtime;
    const auto changed = [&](OrbisPadButtonDataOffset button, Runtime::PadButton gesture) {
        const bool was = True(before & button);
        const bool is = True(after & button);
        if (was != is) {
            Runtime::Instance().NotePadButton(gesture, is);
        }
    };
    changed(OrbisPadButtonDataOffset::Cross, Runtime::PadButton::Cross);
    changed(OrbisPadButtonDataOffset::Options, Runtime::PadButton::Options);
}

void GameController::Button(OrbisPadButtonDataOffset button, bool is_pressed) {
    if (HeadsetPlays()) {
        return;
    }
    const auto before = m_state.buttonsState;
    m_state.OnButton(button, is_pressed);
    PushState();
    if (IsFirstController(this)) {
        NoteViewButtons(before, m_state.buttonsState);
    }
}

void GameController::Axis(Input::Axis axis, int value, bool smooth) {
    if (HeadsetPlays()) {
        return;
    }
    m_state.OnAxis(axis, value, smooth);
    PushState();
}

OrbisPadButtonDataOffset GameController::ApplyRemoteLocked(OrbisPadButtonDataOffset buttons,
                                                           const std::array<int, 6>& axes,
                                                           bool touch_down, float touch_x,
                                                           float touch_y) {
    const OrbisPadButtonDataOffset before = m_state.buttonsState;
    m_connected = true;
    m_connected_count = 1;
    m_state.buttonsState = buttons;
    for (int i = 0; i < std::to_underlying(Axis::AxisMax); ++i) {
        m_state.OnAxis(static_cast<Input::Axis>(i), axes[i], false);
    }
    m_state.OnTouchpad(0, touch_down, touch_x, touch_y);
    m_state.time = Libraries::Kernel::sceKernelGetProcessTime();
    m_states_queue.Push(m_state);
    return before;
}

void GameController::ApplyRemoteState(OrbisPadButtonDataOffset buttons,
                                      const std::array<int, 6>& axes, bool touch_down,
                                      float touch_x, float touch_y) {
    OrbisPadButtonDataOffset before;
    {
        std::lock_guard lock(m_states_queue_mutex);
        before = ApplyRemoteLocked(buttons, axes, touch_down, touch_x, touch_y);
    }
    if (IsFirstController(this)) {
        NoteViewButtons(before, buttons);
    }
}

void GameController::ApplyHeadsetState(OrbisPadButtonDataOffset buttons,
                                       const std::array<int, 6>& axes, bool touch_down,
                                       float touch_x, float touch_y) {
    OrbisPadButtonDataOffset before;
    {
        std::lock_guard lock(m_states_queue_mutex);
        // (The gamepad may have taken the controller back a moment ago, from another thread.)
        if (!HeadsetPlays()) {
            return;
        }
        before = ApplyRemoteLocked(buttons, axes, touch_down, touch_x, touch_y);
    }
    if (IsFirstController(this)) {
        NoteViewButtons(before, buttons);
    }
}

void GameController::SetHeadsetPlays(bool plays) {
    OrbisPadButtonDataOffset before;
    {
        std::lock_guard lock(m_states_queue_mutex);
        if (HeadsetPlays() == plays) {
            return;
        }
        m_headset_plays.store(plays, std::memory_order_relaxed);
        // Whoever played until now leaves nothing held.
        static constexpr std::array<int, 6> Rest{128, 128, 128, 128, 0, 0};
        before = ApplyRemoteLocked({}, Rest, false, 0.5f, 0.5f);
    }
    {
        std::scoped_lock lock{m_finger_mutex};
        m_finger_down = false;
        m_stick_touch = false;
        m_gesture_press = false;
        m_stick_finger.Reset();
        m_gestures.Reset();
        m_view_turn.Reset();
    }
    if (IsFirstController(this)) {
        NoteViewButtons(before, {});
    }
}

void GameController::Gyro(int id) {
    m_state.OnGyro(gyro_buf);
    PushState();
    // This is called every few milliseconds for every controller, whatever it has to say.
    if (IsFirstController(this)) {
        Core::Vr::Runtime::Instance().PollViewGestures();
        UpdateStickTouch();
    }
}

void GameController::UpdateStickTouch() {
    // A controller without a touchpad of its own (or whose touchpad nobody touches): the right
    // stick moves a finger over it (see StickFinger for how). Titles made for a headset have
    // their players swipe and drag, and have no use for a second stick.
    // SHADPS4_STICK_TOUCHPAD=0 leaves the stick a stick.
    static const bool enabled = [] {
        const char* value = std::getenv("SHADPS4_STICK_TOUCHPAD");
        return value == nullptr || value[0] != '0';
    }();
    if (m_sdl_gamepad == nullptr || !Core::Vr::Runtime::Instance().IsHeadsetConnected() ||
        HeadsetPlays()) {
        return;
    }
    std::scoped_lock lock{m_finger_mutex};
    // The left shoulder button, which such a title has no use for in play, held: each flick
    // of the right stick to a side turns the view a step that way (see ViewTurn), for players
    // who cannot turn round where they sit. The stick moves no finger meanwhile. (Not while
    // the PS button is held: it gives the shoulder buttons another meaning.)
    const bool turning = Common::ElfInfo::Instance().GameSerial() == "CUSA12392" &&
        SDL_GetGamepadButton(m_sdl_gamepad, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER) &&
        !SDL_GetGamepadButton(m_sdl_gamepad, SDL_GAMEPAD_BUTTON_GUIDE);
    if (const int steps = m_view_turn.Update(
            turning, static_cast<float>(SDL_GetGamepadAxis(m_sdl_gamepad, SDL_GAMEPAD_AXIS_RIGHTX)) /
                         32767.0f);
        steps != 0) {
        Core::Vr::Runtime::Instance().TurnView(steps);
    }
    if (m_finger_down) {
        // A real finger is on the pad: the stick's has nothing to say, and is not back on
        // the pad the moment the real one lifts either.
        m_stick_finger.Reset();
        return;
    }
    const double now =
        std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
    // A gamepad that has no touchpad at all also has buttons for what such a title wants
    // done on one (see PadGestures): the right trigger presses the pad for as long as it is
    // pulled, the right shoulder button swipes forward once, the left trigger pulls back and
    // lets go when it is let go. The title has no use for those buttons in play, and is
    // still told of them. (Not while the PS button is held: it gives them other meanings.)
    PadGestures::Touch made{};
    if (Common::ElfInfo::Instance().GameSerial() == "CUSA12392" &&
        SDL_GetNumGamepadTouchpads(m_sdl_gamepad) == 0) {
        PadGestures::Controls controls;
        if (!SDL_GetGamepadButton(m_sdl_gamepad, SDL_GAMEPAD_BUTTON_GUIDE)) {
            static constexpr s16 Pulled = 16384;
            controls.press =
                SDL_GetGamepadAxis(m_sdl_gamepad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) > Pulled;
            controls.swipe =
                SDL_GetGamepadButton(m_sdl_gamepad, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER);
            controls.pull =
                SDL_GetGamepadAxis(m_sdl_gamepad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) > Pulled;
        }
        made = m_gestures.Update(now, controls);
        if (made.pressed != m_gesture_press) {
            m_gesture_press = made.pressed;
            Button(OrbisPadButtonDataOffset::TouchPad, made.pressed);
        }
    }
    StickFinger::Touch touch{false, m_stick_touch_x, m_stick_touch_y};
    if (made.down) {
        m_stick_finger.Reset();
        touch = {true, made.x, made.y};
    } else if (turning) {
        m_stick_finger.Reset();
    } else if (enabled) {
        const float x = static_cast<float>(SDL_GetGamepadAxis(m_sdl_gamepad, SDL_GAMEPAD_AXIS_RIGHTX)) /
                        32767.0f;
        const float y = static_cast<float>(SDL_GetGamepadAxis(m_sdl_gamepad, SDL_GAMEPAD_AXIS_RIGHTY)) /
                        32767.0f;
        touch = m_stick_finger.Update(now, x, y);
    }
    if (touch.down) {
        if (!m_stick_touch || std::abs(touch.x - m_stick_touch_x) > 0.002f ||
            std::abs(touch.y - m_stick_touch_y) > 0.002f) {
            m_stick_touch_x = touch.x;
            m_stick_touch_y = touch.y;
            ApplyTouch(0, true, touch.x, touch.y);
        }
    } else if (m_stick_touch) {
        ApplyTouch(0, false, m_stick_touch_x, m_stick_touch_y);
    }
    m_stick_touch = touch.down;
}

void GameController::Acceleration(int id) {
    m_state.OnAccel(accel_buf);
    PushState();
}

void GameController::UpdateGyro(const float gyro[3]) {
    std::scoped_lock l(m_states_queue_mutex);
    std::memcpy(gyro_buf, gyro, sizeof(gyro_buf));
}

void GameController::UpdateAcceleration(const float acceleration[3]) {
    std::scoped_lock l(m_states_queue_mutex);
    std::memcpy(accel_buf, acceleration, sizeof(accel_buf));
}

void GameController::UpdateAxisSmoothing() {
    m_state.UpdateAxisSmoothing();
}

void GameController::SetLightBarRGB(u8 const r, u8 const g, u8 const b) {
    if (override_colour.has_value()) {
        return;
    }
    colour = {r, g, b};
    if (IsFirstController(this)) {
        Core::Vr::Runtime::Instance().SetPadLight(r, g, b);
    }
    if (m_sdl_gamepad != nullptr) {
        SDL_SetGamepadLED(m_sdl_gamepad, r, g, b);
    }
}

void GameController::SetLightBarRGB(Colour const c) {
    SetLightBarRGB(c.r, c.g, c.b);
}

Colour GameController::GetLightBarRGB() {
    return colour;
}

void GameController::PollLightColour() {
    if (m_sdl_gamepad != nullptr) {
        SDL_SetGamepadLED(m_sdl_gamepad, colour.r, colour.g, colour.b);
    }
}

void GameControllers::ResetLightbarColors() {
    for (auto& c : controllers) {
        auto const* u = UserManagement.GetUserByID(c->user_id);
        if (!u || !c->m_sdl_gamepad) {
            continue;
        }
        auto const i = u->user_color - 1;
        if (i < 0 || i > 3) {
            continue;
        }
        auto const& col = g_user_colours[i];
        c->override_colour = std::nullopt;
        c->SetLightBarRGB(col);
    }
}

bool GameController::SetVibration(u8 smallMotor, u8 largeMotor) {
    if (IsFirstController(this)) {
        Core::Vr::Runtime::Instance().SetPadVibration(smallMotor, largeMotor);
    }
    // (A gamepad that lies on the desk while the headset's controllers play stays still.)
    if (m_sdl_gamepad != nullptr && !HeadsetPlays()) {
        return SDL_RumbleGamepad(m_sdl_gamepad, (smallMotor / 255.0f) * 0xFFFF,
                                 (largeMotor / 255.0f) * 0xFFFF, -1);
    }
    return true;
}

void GameController::SetTouchpadState(int touchIndex, bool touchDown, float x, float y) {
    if (HeadsetPlays()) {
        return;
    }
    // A finger on the real touchpad: the right stick stops standing in for one. (Its finger
    // lifts first: the real one coming down is a touch of its own, not that one moving.)
    std::scoped_lock lock{m_finger_mutex};
    if (touchIndex == 0) {
        if (m_stick_touch) {
            ApplyTouch(0, false, m_stick_touch_x, m_stick_touch_y);
            m_stick_touch = false;
        }
        if (touchDown && !m_finger_down && !m_touchpad_noted) {
            m_touchpad_noted = true;
            LOG_INFO(Input, "The controller's touchpad feels a finger");
        }
        m_finger_down = touchDown;
        m_stick_finger.Reset();
    }
    ApplyTouch(touchIndex, touchDown, x, y);
}

void GameController::ApplyTouch(int touchIndex, bool touchDown, float x, float y) {
    if (touchIndex < 2) {
        bool was_pressed = m_state.touchpad[0].state || m_state.touchpad[1].state;
        m_state.OnTouchpad(touchIndex, touchDown, x, y);
        PushState();
        if (!m_state.touchpad[0].state && !m_state.touchpad[1].state && was_pressed) {
            last_touch_down_timestamp = 0;
        } else if ((m_state.touchpad[0].state || m_state.touchpad[1].state) && !was_pressed) {
            last_touch_down_timestamp = m_state.time;
        }
    }
}

void GameControllers::CalculateOrientation(Libraries::Pad::OrbisFVector3& acceleration,
                                           Libraries::Pad::OrbisFVector3& angularVelocity,
                                           float deltaTime,
                                           Libraries::Pad::OrbisFQuaternion& lastOrientation,
                                           Libraries::Pad::OrbisFQuaternion& orientation) {
    // avoid wildly off values coming from elapsed time between two samples
    // being too high, such as on the first time the controller is polled
    if (deltaTime > 1.0f) {
        orientation = lastOrientation;
        return;
    }
    Libraries::Pad::OrbisFQuaternion q = lastOrientation;
    Libraries::Pad::OrbisFQuaternion ω = {angularVelocity.x, angularVelocity.y, angularVelocity.z,
                                          0.0f};

    Libraries::Pad::OrbisFQuaternion qω = {q.w * ω.x + q.x * ω.w + q.y * ω.z - q.z * ω.y,
                                           q.w * ω.y + q.y * ω.w + q.z * ω.x - q.x * ω.z,
                                           q.w * ω.z + q.z * ω.w + q.x * ω.y - q.y * ω.x,
                                           q.w * ω.w - q.x * ω.x - q.y * ω.y - q.z * ω.z};

    Libraries::Pad::OrbisFQuaternion qDot = {0.5f * qω.x, 0.5f * qω.y, 0.5f * qω.z, 0.5f * qω.w};

    q.x += qDot.x * deltaTime;
    q.y += qDot.y * deltaTime;
    q.z += qDot.z * deltaTime;
    q.w += qDot.w * deltaTime;

    float norm = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
    q.x /= norm;
    q.y /= norm;
    q.z /= norm;
    q.w /= norm;

    orientation.x = q.x;
    orientation.y = q.y;
    orientation.z = q.z;
    orientation.w = q.w;
    LOG_DEBUG(Lib_Pad, "Calculated orientation: {:.2f} {:.2f} {:.2f} {:.2f}", orientation.x,
              orientation.y, orientation.z, orientation.w);
}

void GameController::ConnectController(SDL_Gamepad* pad) {
    m_sdl_gamepad = pad;
    m_connected_count = 1;
    m_connected = true;
}
void GameController::DisconnectController() {
    m_sdl_gamepad = nullptr;
    m_connected_count = 0;
    m_connected = false;
}

bool is_first_check = true;

void GameControllers::TryOpenSDLControllers() {
    using namespace Libraries::UserService;
    int controller_count;
    s32 move_count = 0;
    SDL_JoystickID* new_joysticks = SDL_GetGamepads(&controller_count);
    LOG_INFO(Input, "{} controllers are currently connected", controller_count);

    std::unordered_set<SDL_JoystickID> assigned_ids;
    std::array<bool, 4> slot_taken{false, false, false, false};

    // The first player gets the best of what is there: a DualSense before other PlayStation
    // controllers (among which is the copy of a gamepad paired with a headset that Virtual
    // Desktop puts on the PC, a DualShock 4 without motion sensors or touchpad), those before
    // anything else.
    const auto rank = [](SDL_JoystickID id) {
        const u16 vendor = SDL_GetGamepadVendorForID(id);
        const u16 product = SDL_GetGamepadProductForID(id);
        if (vendor == 0x054c && (product == 0x0ce6 || product == 0x0df2)) {
            return 0;
        }
        return vendor == 0x054c ? 1 : 2;
    };
    std::stable_sort(new_joysticks, new_joysticks + controller_count,
                     [&](SDL_JoystickID a, SDL_JoystickID b) { return rank(a) < rank(b); });

    for (int i = 0; i < 4; i++) {
        SDL_Gamepad* pad = controllers[i]->m_sdl_gamepad;
        if (pad) {
            SDL_JoystickID id = SDL_GetGamepadID(pad);
            bool still_connected = false;
            ControllerType type = ControllerType::Standard;
            for (int j = 0; j < controller_count; j++) {
                if (new_joysticks[j] == id) {
                    still_connected = true;
                    assigned_ids.insert(id);
                    slot_taken[i] = true;
                    break;
                }
            }
            if (!still_connected) {
                auto u = UserManagement.GetUserByID(controllers[i]->user_id);
                SDL_CloseGamepad(pad);
                controllers[i]->DisconnectController();
                controllers[i]->user_id = -1;
                slot_taken[i] = false;
            }
        }
    }

    // In a headset there is one player: the best gamepad is theirs, the others stay unused.
    // (Each would log in a player of its own, and a title made for one player in a headset may
    // not cope: Astro Bot stops when its second player's controller cannot be tracked. Virtual
    // Desktop adds a gamepad of its own, made from the headset's controllers.)
    // (SHADPS4_VR_ONE_PLAYER=0: every gamepad logs in a player of its own all the same.)
    static const bool one_player_wanted = [] {
        const char* value = std::getenv("SHADPS4_VR_ONE_PLAYER");
        return value == nullptr || value[0] != '0';
    }();
    const bool one_player =
        one_player_wanted && Core::Vr::Runtime::Instance().IsHeadsetConnected();
    static std::unordered_set<SDL_JoystickID> left_unused;
    for (int j = 0; j < controller_count; j++) {
        SDL_JoystickID id = new_joysticks[j];
        if (assigned_ids.contains(id))
            continue;
        if (one_player && (slot_taken[0] || controllers[0]->m_sdl_gamepad != nullptr)) {
            if (left_unused.insert(id).second) {
                const char* name = SDL_GetGamepadNameForID(id);
                LOG_INFO(Input,
                         "Controller {} ({:04x}:{:04x}) is not used: in the headset there is one "
                         "player, and another controller is theirs",
                         name != nullptr ? name : "unnamed", SDL_GetGamepadVendorForID(id),
                         SDL_GetGamepadProductForID(id));
            }
            continue;
        }
        left_unused.erase(id);

        SDL_Gamepad* pad = SDL_OpenGamepad(id);
        if (!pad) {
            static std::unordered_set<SDL_JoystickID> refused;
            if (refused.insert(id).second) {
                const char* name = SDL_GetGamepadNameForID(id);
                LOG_WARNING(Input,
                            "Controller {} ({:04x}:{:04x}) cannot be opened and is not used: {}",
                            name != nullptr ? name : "unnamed", SDL_GetGamepadVendorForID(id),
                            SDL_GetGamepadProductForID(id), SDL_GetError());
            }
            continue;
        }

        for (int i = 0; i < 4; i++) {
            if (!slot_taken[i]) {
                auto u = UserManagement.GetUserByPlayerIndex(i + 1);
                if (!u) {
                    LOG_INFO(Input, "User {} not found", i + 1);
                    continue; // for now, if you don't specify who Player N is in the config,
                              // Player N won't be registered at all
                }
                auto* c = controllers[i];
                LOG_INFO(Input, "Gamepad registered for slot {}! Handle: {}", i,
                         SDL_GetGamepadID(pad));
                slot_taken[i] = true;
                c->user_id = u->user_id;
                UserManagement.LoginUser(u, i + 1);
                c->ConnectController(pad);
                {
                    const char* name = SDL_GetGamepadName(pad);
                    const char* path = SDL_GetGamepadPath(pad);
                    LOG_INFO(Input,
                             "Controller {} connected: {} ({:04x}:{:04x} at {}; motion sensors {}, "
                             "touchpad {}, light {})",
                             i + 1, name != nullptr ? name : "unnamed", SDL_GetGamepadVendor(pad),
                             SDL_GetGamepadProduct(pad), path != nullptr ? path : "?",
                             SDL_GamepadHasSensor(pad, SDL_SENSOR_GYRO) ? "yes" : "no",
                             SDL_GetNumGamepadTouchpads(pad) > 0
                                 ? "yes"
                                 : "no: the right stick stands in for a finger, the right "
                                   "trigger presses the pad, the right shoulder button swipes "
                                   "forward, the left trigger pulls back and lets go",
                             SDL_GetBooleanProperty(SDL_GetGamepadProperties(pad),
                                                    SDL_PROP_GAMEPAD_CAP_RGB_LED_BOOLEAN, false)
                                 ? "yes"
                                 : "no");
                }
                if (EmulatorSettings.IsMotionControlsEnabled()) {
                    if (SDL_SetGamepadSensorEnabled(c->m_sdl_gamepad, SDL_SENSOR_GYRO, true)) {
                        c->gyro_poll_rate =
                            SDL_GetGamepadSensorDataRate(c->m_sdl_gamepad, SDL_SENSOR_GYRO);
                        LOG_INFO(Input, "Gyro initialized, poll rate: {}", c->gyro_poll_rate);
                    } else {
                        LOG_ERROR(Input, "Failed to initialize gyro controls for gamepad {}",
                                  c->user_id);
                    }
                    if (SDL_SetGamepadSensorEnabled(c->m_sdl_gamepad, SDL_SENSOR_ACCEL, true)) {
                        c->accel_poll_rate =
                            SDL_GetGamepadSensorDataRate(c->m_sdl_gamepad, SDL_SENSOR_ACCEL);
                        LOG_INFO(Input, "Accel initialized, poll rate: {}", c->accel_poll_rate);
                    } else {
                        LOG_ERROR(Input, "Failed to initialize accel controls for gamepad {}",
                                  c->user_id);
                    }
                }
                break;
            }
        }
    }
    if (is_first_check) [[unlikely]] {
        is_first_check = false;
        // The first player is logged in from the start, gamepad or not. (Counting the gamepads
        // there are is not enough: one that is listed and cannot be opened, as a virtual one
        // that is just going away, left nobody logged in, and a title that takes its first
        // player for granted stopped right at its start.)
        if (controllers[0]->user_id == Libraries::UserService::ORBIS_USER_SERVICE_USER_ID_INVALID) {
            if (auto u = UserManagement.GetUserByPlayerIndex(1); u != nullptr) {
                controllers[0]->user_id = u->user_id;
                controllers[0]->ConnectController(nullptr);
                UserManagement.LoginUser(u, 1);
            }
        }
    }
    SDL_free(new_joysticks);
}
u8 GameController::GetTouchCount() {
    return m_touch_count;
}

void GameController::SetTouchCount(u8 touchCount) {
    m_touch_count = touchCount;
}

u8 GameController::GetSecondaryTouchCount() {
    return m_secondary_touch_count;
}

void GameController::SetSecondaryTouchCount(u8 touchCount) {
    m_secondary_touch_count = touchCount;
    if (touchCount == 0) {
        m_was_secondary_reset = true;
    }
}

u8 GameController::GetPreviousTouchNum() {
    return m_previous_touchnum;
}

void GameController::SetPreviousTouchNum(u8 touchNum) {
    m_previous_touchnum = touchNum;
}

bool GameController::WasSecondaryTouchReset() {
    return m_was_secondary_reset;
}

void GameController::UnsetSecondaryTouchResetBool() {
    m_was_secondary_reset = false;
}

void GameController::SetLastOrientation(Libraries::Pad::OrbisFQuaternion& orientation) {
    m_orientation = orientation;
}

Libraries::Pad::OrbisFQuaternion GameController::GetLastOrientation() {
    return m_orientation;
}

std::chrono::steady_clock::time_point GameController::GetLastUpdate() {
    return m_last_update;
}

void GameController::SetLastUpdate(std::chrono::steady_clock::time_point lastUpdate) {
    m_last_update = lastUpdate;
}

void GameController::PushState() {
    std::lock_guard lg(m_states_queue_mutex);
    m_state.time = Libraries::Kernel::sceKernelGetProcessTime();
    m_states_queue.Push(m_state);
}

u8 GameControllers::GetGamepadIndexFromJoystickId(SDL_JoystickID id) {
    auto g = SDL_GetGamepadFromID(id);
    ASSERT(g != nullptr);
    for (int i = 0; i < 5; i++) {
        if (controllers[i]->m_sdl_gamepad == g) {
            return i;
        }
    }
    // LOG_TRACE(Input, "Gamepad index: {}", index);
    return -1;
}

std::optional<u8> GameControllers::GetControllerIndexFromUserID(s32 user_id) {
    auto const u = UserManagement.GetUserByID(user_id);
    if (!u) {
        return std::nullopt;
    }
    return u->player_index - 1;
}

std::optional<u8> GameControllers::GetControllerIndexFromControllerID(s32 controller_id) {
    if (controller_id < 1 || controller_id > 5) {
        return std::nullopt;
    }
    return controller_id - 1;
}

} // namespace Input
