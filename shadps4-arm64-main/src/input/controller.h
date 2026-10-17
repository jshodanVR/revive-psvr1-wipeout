// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <atomic>
#include <mutex>
#include <utility>
#include <vector>

#include <SDL3/SDL_gamepad.h>
#include "SDL3/SDL_joystick.h"
#include "common/assert.h"
#include "common/ring_buffer_queue.h"
#include "core/libraries/pad/pad.h"
#include "core/libraries/system/userservice.h"
#include "input/pad_gestures.h"
#include "input/stick_finger.h"
#include "input/view_turn.h"

struct SDL_Gamepad;

namespace Input {

enum class ControllerType {
    Standard,
};

enum class Axis {
    LeftX = 0,
    LeftY = 1,
    RightX = 2,
    RightY = 3,
    TriggerLeft = 4,
    TriggerRight = 5,

    AxisMax
};

struct TouchpadEntry {
    u8 ID = 0;
    bool state{};
    u16 x{};
    u16 y{};
};

struct Colour {
    u8 r, g, b;
};
static constexpr Input::Colour g_user_colours[4]{
    {0, 0, 255},   // blue
    {255, 0, 0},   // red
    {0, 255, 0},   // green
    {255, 0, 255}, // pink
};

struct State {
private:
    template <typename T>
    using AxisArray = std::array<T, std::to_underlying(Axis::AxisMax)>;
    static constexpr AxisArray<s32> axis_defaults{128, 128, 128, 128, 0, 0};
    static constexpr u64 axis_smoothing_time{33000};
    AxisArray<bool> axis_smoothing_flags{true};
    AxisArray<u64> axis_smoothing_start_times{0};
    AxisArray<int> axis_smoothing_start_values{axis_defaults};
    AxisArray<int> axis_smoothing_end_values{axis_defaults};

public:
    void OnButton(Libraries::Pad::OrbisPadButtonDataOffset, bool);
    void OnAxis(Axis, int, bool smooth = true);
    void OnTouchpad(int touchIndex, bool isDown, float x, float y);
    void OnGyro(const float[3]);
    void OnAccel(const float[3]);
    void UpdateAxisSmoothing();

    Libraries::Pad::OrbisPadButtonDataOffset buttonsState{};
    u64 time = 0;
    AxisArray<s32> axes{axis_defaults};
    TouchpadEntry touchpad[2] = {{false, 0, 0}, {false, 0, 0}};
    Libraries::Pad::OrbisFVector3 acceleration = {0.0f, -9.81f, 0.0f};
    Libraries::Pad::OrbisFVector3 angularVelocity = {0.0f, 0.0f, 0.0f};
    Libraries::Pad::OrbisFQuaternion orientation = {0.0f, 0.0f, 0.0f, 1.0f};
};

inline int GetAxis(int min, int max, int value) {
    int v = (255 * (value - min)) / (max - min);
    return (v < 0 ? 0 : (v > 255 ? 255 : v));
}

class GameController {
    friend class GameControllers;

public:
    GameController();
    virtual ~GameController() = default;
    void ConnectController(SDL_Gamepad* pad);
    void DisconnectController();

    void ReadState(State* state, bool* isConnected, int* connectedCount);
    int ReadStates(State* states, int states_num, bool* isConnected, int* connectedCount);

    void Button(Libraries::Pad::OrbisPadButtonDataOffset button, bool isPressed);
    void Axis(Input::Axis axis, int value, bool smooth = true);
    void ApplyRemoteState(Libraries::Pad::OrbisPadButtonDataOffset buttons,
                          const std::array<int, 6>& axes, bool touch_down, float touch_x,
                          float touch_y);
    /// In a headset, the headset's own controllers can play this controller in place of the
    /// PC's gamepad (see Input::PadSource). While they do, what the gamepad's buttons, sticks
    /// and touchpad say is left aside. The controller is at rest after either change.
    void SetHeadsetPlays(bool plays);
    bool HeadsetPlays() const {
        return m_headset_plays.load(std::memory_order_relaxed);
    }
    /// ApplyRemoteState for the headset's controllers: nothing unless they play.
    void ApplyHeadsetState(Libraries::Pad::OrbisPadButtonDataOffset buttons,
                           const std::array<int, 6>& axes, bool touch_down, float touch_x,
                           float touch_y);
    void Gyro(int id);
    void Acceleration(int id);
    void UpdateGyro(const float gyro[3]);
    void UpdateAcceleration(const float acceleration[3]);
    void UpdateAxisSmoothing();
    void SetLightBarRGB(u8 const r, u8 const g, u8 const b);
    void SetLightBarRGB(Colour const c);
    Colour GetLightBarRGB();
    void PollLightColour();
    bool SetVibration(u8 smallMotor, u8 largeMotor);
    void SetTouchpadState(int touchIndex, bool touchDown, float x, float y);

    u8 GetTouchCount();
    void SetTouchCount(u8 touchCount);
    u8 GetSecondaryTouchCount();
    void SetSecondaryTouchCount(u8 touchCount);
    u8 GetPreviousTouchNum();
    void SetPreviousTouchNum(u8 touchNum);
    bool WasSecondaryTouchReset();
    void UnsetSecondaryTouchResetBool();

    void SetLastOrientation(Libraries::Pad::OrbisFQuaternion& orientation);
    Libraries::Pad::OrbisFQuaternion GetLastOrientation();
    std::chrono::steady_clock::time_point GetLastUpdate();
    void SetLastUpdate(std::chrono::steady_clock::time_point lastUpdate);

    float gyro_poll_rate;
    float accel_poll_rate;
    float gyro_buf[3] = {0.0f, 0.0f, 0.0f}, accel_buf[3] = {0.0f, 9.81f, 0.0f};
    s32 user_id = Libraries::UserService::ORBIS_USER_SERVICE_USER_ID_INVALID;
    SDL_Gamepad* m_sdl_gamepad = nullptr;
    u64 last_touch_down_timestamp = 0;

private:
    void PushState();
    /// With m_states_queue_mutex held. The buttons that were down before.
    Libraries::Pad::OrbisPadButtonDataOffset ApplyRemoteLocked(
        Libraries::Pad::OrbisPadButtonDataOffset buttons, const std::array<int, 6>& axes,
        bool touch_down, float touch_x, float touch_y);
    void ApplyTouch(int touchIndex, bool touchDown, float x, float y);
    /// Lets the right stick stand in for a finger on the touchpad (see controller.cpp).
    void UpdateStickTouch();

    // The touchpad's first finger is the real one's (the window's events) or the right
    // stick's (a timer): what says which is kept together.
    std::mutex m_finger_mutex;
    bool m_finger_down = false;
    bool m_touchpad_noted = false;
    StickFinger m_stick_finger;
    // On a gamepad without a touchpad, buttons do its gestures as well (see PadGestures).
    PadGestures m_gestures;
    bool m_gesture_press = false;
    // The left shoulder button held, the stick turns the view instead (see ViewTurn).
    ViewTurn m_view_turn;
    bool m_stick_touch = false;
    float m_stick_touch_x = 0.5f;
    float m_stick_touch_y = 0.5f;
    std::atomic<bool> m_headset_plays{false};
    bool m_connected = false;
    int m_connected_count = 0;
    u8 m_touch_count = 0;
    u8 m_secondary_touch_count = 0;
    u8 m_previous_touchnum = 0;
    bool m_was_secondary_reset = false;
    std::chrono::steady_clock::time_point m_last_update = {};
    Libraries::Pad::OrbisFQuaternion m_orientation = {0.0f, 0.0f, 0.0f, 1.0f};
    Colour colour;
    std::optional<Colour> override_colour{};

    State m_state;

    std::mutex m_states_queue_mutex;
    RingBufferQueue<State> m_states_queue;
};

class GameControllers {
    std::array<GameController*, 5> controllers;

public:
    GameControllers()
        : controllers({new GameController(), new GameController(), new GameController(),
                       new GameController(), new GameController()}) {};
    virtual ~GameControllers() = default;
    GameController* operator[](const size_t& i) const {
        if (i > 4) {
            UNREACHABLE_MSG("Index {} is out of bounds for GameControllers!", i);
        }
        return controllers[i];
    }
    void TryOpenSDLControllers();
    u8 GetGamepadIndexFromJoystickId(SDL_JoystickID id);
    static std::optional<u8> GetControllerIndexFromUserID(s32 user_id);
    static std::optional<u8> GetControllerIndexFromControllerID(s32 controller_id);

    static void CalculateOrientation(Libraries::Pad::OrbisFVector3& acceleration,
                                     Libraries::Pad::OrbisFVector3& angularVelocity,
                                     float deltaTime,
                                     Libraries::Pad::OrbisFQuaternion& lastOrientation,
                                     Libraries::Pad::OrbisFQuaternion& orientation);
    void SetControllerCustomColor(s32 i, u8 r, u8 g, u8 b) {
        // reset to ensure the next function always runs, even if there already was a preexisting
        // override colour before
        controllers[i]->override_colour = std::nullopt;
        controllers[i]->SetLightBarRGB(r, g, b);
        controllers[i]->override_colour = {r, g, b};
    }
    void ResetLightbarColors();
};

} // namespace Input
