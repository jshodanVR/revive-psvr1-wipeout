// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <SDL3/SDL_events.h>
#include <SDL3/SDL_hints.h>
#include <SDL3/SDL_init.h>
#include <SDL3/SDL_properties.h>
#include <SDL3/SDL_timer.h>
#include <SDL3/SDL_video.h>
#include <cmrc/cmrc.hpp>
#include <stb_image.h>

#include "common/assert.h"
#include "common/elf_info.h"
#include "common/io_file.h"
#include "common/logging/formatter.h"
#include "common/scope_exit.h"
#include "core/debug_state.h"
#include "core/devtools/layer.h"
#include "core/emulator_settings.h"
#include "core/libraries/kernel/time.h"
#include "core/libraries/pad/pad.h"
#include "core/libraries/system/userservice.h"
#include "core/user_settings.h"
#include "imgui/renderer/imgui_core.h"
#include "input/controller.h"
#include "input/scripted_input.h"
#include "input/input_handler.h"
#include "input/input_mouse.h"
#include "input/pad_source.h"
#include "core/vr/vr_runtime.h"
#include "sdl_window.h"
#include "video_core/renderdoc.h"

#ifdef __APPLE__
#include <SDL3/SDL_metal.h>
#endif
#include <core/emulator_settings.h>
#include "core/libraries/mouse/sdl_mouse.h"

CMRC_DECLARE(res);

namespace Frontend {

using namespace Libraries::Pad;

static OrbisPadButtonDataOffset SDLGamepadToOrbisButton(u8 button) {
    using OPBDO = OrbisPadButtonDataOffset;

    switch (button) {
    case SDL_GAMEPAD_BUTTON_DPAD_DOWN:
        return OPBDO::Down;
    case SDL_GAMEPAD_BUTTON_DPAD_UP:
        return OPBDO::Up;
    case SDL_GAMEPAD_BUTTON_DPAD_LEFT:
        return OPBDO::Left;
    case SDL_GAMEPAD_BUTTON_DPAD_RIGHT:
        return OPBDO::Right;
    case SDL_GAMEPAD_BUTTON_SOUTH:
        return OPBDO::Cross;
    case SDL_GAMEPAD_BUTTON_NORTH:
        return OPBDO::Triangle;
    case SDL_GAMEPAD_BUTTON_WEST:
        return OPBDO::Square;
    case SDL_GAMEPAD_BUTTON_EAST:
        return OPBDO::Circle;
    case SDL_GAMEPAD_BUTTON_START:
        return OPBDO::Options;
    case SDL_GAMEPAD_BUTTON_TOUCHPAD:
        return OPBDO::TouchPad;
    case SDL_GAMEPAD_BUTTON_BACK:
        return OPBDO::TouchPad;
    case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER:
        return OPBDO::L1;
    case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER:
        return OPBDO::R1;
    case SDL_GAMEPAD_BUTTON_LEFT_STICK:
        return OPBDO::L3;
    case SDL_GAMEPAD_BUTTON_RIGHT_STICK:
        return OPBDO::R3;
    default:
        return OPBDO::None;
    }
}

static Uint32 SDLCALL PollController(void* userdata, SDL_TimerID timer_id, Uint32 interval) {
    auto* controller = reinterpret_cast<Input::GameController*>(userdata);
    controller->UpdateAxisSmoothing();
    controller->Gyro(0);
    controller->Acceleration(0);
    return interval;
}

static Uint32 SDLCALL PollControllerLightColour(void* userdata, SDL_TimerID timer_id,
                                                Uint32 interval) {
    auto* controller = reinterpret_cast<Input::GameController*>(userdata);
    controller->PollLightColour();
    return interval;
}

WindowSDL::WindowSDL(s32 width_, s32 height_, Input::GameControllers* controllers_,
                     std::string_view window_title)
    : width{width_}, height{height_}, controllers{*controllers_} {
    if (!SDL_SetHint(SDL_HINT_APP_NAME, "shadPS4")) {
        UNREACHABLE_MSG("Failed to set SDL window hint: {}", SDL_GetError());
    }
    // With SHADPS4_HEADLESS there is no display to show the game on: the frames go to a VR host
    // (or nowhere), and SDL only has to provide events.
    const bool headless = std::getenv("SHADPS4_HEADLESS") != nullptr;
    if (headless) {
        SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "dummy");
    }
    LOG_INFO(Input, "Initializing SDL video subsystem");
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        UNREACHABLE_MSG("Failed to initialize SDL video subsystem: {}", SDL_GetError());
    }
    LOG_INFO(Input, "SDL video subsystem initialized");
    if (!SDL_Init(SDL_INIT_CAMERA)) {
        LOG_ERROR(Input, "Failed to initialize SDL camera subsystem: {}", SDL_GetError());
    }
    SDL_InitSubSystem(SDL_INIT_AUDIO);

    SDL_PropertiesID props = SDL_CreateProperties();
    SDL_SetStringProperty(props, SDL_PROP_WINDOW_CREATE_TITLE_STRING,
                          std::string(window_title).c_str());
#ifdef ENABLE_BACHATA_RUNTIME
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_X_NUMBER, 0);
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_Y_NUMBER, 0);
#else
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_X_NUMBER, SDL_WINDOWPOS_CENTERED);
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_Y_NUMBER, SDL_WINDOWPOS_CENTERED);
#endif
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_WIDTH_NUMBER, width);
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_HEIGHT_NUMBER, height);
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_FLAGS_NUMBER,
                          headless ? 0 : SDL_WINDOW_VULKAN);
    SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_RESIZABLE_BOOLEAN, true);
    LOG_INFO(Input, "Creating SDL Vulkan window");
    window = SDL_CreateWindowWithProperties(props);
    LOG_INFO(Input, "SDL Vulkan window creation returned");
    SDL_DestroyProperties(props);
    if (window == nullptr) {
        UNREACHABLE_MSG("Failed to create window handle: {}", SDL_GetError());
    }

    SDL_SetWindowMinimumSize(window, 640, 360);

    bool error = false;
    const SDL_DisplayID displayIndex = SDL_GetDisplayForWindow(window);
    if (displayIndex < 0) {
        LOG_ERROR(Frontend, "Error getting display index: {}", SDL_GetError());
        error = true;
    }
    const SDL_DisplayMode* displayMode;
    if ((displayMode = SDL_GetCurrentDisplayMode(displayIndex)) == 0) {
        LOG_ERROR(Frontend, "Error getting display mode: {}", SDL_GetError());
        error = true;
    }
    if (!error) {
        SDL_SetWindowFullscreenMode(
            window, EmulatorSettings.GetFullScreenMode() == "Fullscreen" ? displayMode : NULL);
    }
    SDL_SetWindowFullscreen(window, EmulatorSettings.IsFullScreen());
    SDL_SyncWindow(window);

#ifndef ENABLE_BACHATA_RUNTIME
    SDL_InitSubSystem(SDL_INIT_GAMEPAD);
    // SHADPS4_TEST_VIRTUAL_GAMEPADS=<n>, for tests: that many gamepads that nobody holds, an
    // Xbox controller first and DualSenses after it, as if they were plugged in.
    if (const char* count = std::getenv("SHADPS4_TEST_VIRTUAL_GAMEPADS"); count != nullptr) {
        for (int i = 0; i < std::clamp(std::atoi(count), 0, 4); ++i) {
            SDL_VirtualJoystickDesc desc;
            SDL_INIT_INTERFACE(&desc);
            desc.type = SDL_JOYSTICK_TYPE_GAMEPAD;
            desc.naxes = SDL_GAMEPAD_AXIS_COUNT;
            desc.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
            desc.axis_mask = (1u << SDL_GAMEPAD_AXIS_COUNT) - 1;
            desc.button_mask = (1u << SDL_GAMEPAD_BUTTON_COUNT) - 1;
            desc.vendor_id = i == 0 ? 0x045e : 0x054c;
            desc.product_id = i == 0 ? 0x028e : 0x0ce6;
            desc.name = i == 0 ? "Test Xbox 360 Controller" : "Test DualSense";
            const SDL_JoystickID id = SDL_AttachVirtualJoystick(&desc);
            LOG_INFO(Input, "Test gamepad {} plugged in: {}", desc.name, id != 0 ? "yes" : SDL_GetError());
            // SHADPS4_TEST_VIRTUAL_PRESS=<seconds>[,<seconds>...]: the first of them has its
            // cross button pressed for a moment, so long after the start.
            // SHADPS4_TEST_VIRTUAL_TURN=<seconds>[,...]: its left shoulder button held and
            // its right stick flicked to the right (to the left for a negative time).
            if (i == 0 && id != 0) {
                static SDL_Joystick* test_pad = nullptr;
                test_pad = SDL_OpenJoystick(id);
                const auto each = [](const char* name, auto&& with) {
                    const char* at = std::getenv(name);
                    while (at != nullptr && *at != '\0') {
                        with(std::atof(at));
                        at = std::strchr(at, ',');
                        at = at != nullptr ? at + 1 : nullptr;
                    }
                };
                const auto later = [](double seconds, SDL_TimerCallback what) {
                    if (SDL_AddTimer(static_cast<Uint32>(seconds * 1000.0), what, nullptr) == 0) {
                        LOG_ERROR(Input, "Test gamepad: no timer: {}", SDL_GetError());
                    }
                };
                each("SHADPS4_TEST_VIRTUAL_PRESS", [&](double seconds) {
                    later(seconds, [](void*, SDL_TimerID, Uint32) -> Uint32 {
                        const bool done =
                            SDL_SetJoystickVirtualButton(test_pad, SDL_GAMEPAD_BUTTON_SOUTH, true);
                        LOG_INFO(Input, "Test gamepad: cross pressed: {}", done ? "yes" : SDL_GetError());
                        return 0;
                    });
                    later(seconds + 0.3, [](void*, SDL_TimerID, Uint32) -> Uint32 {
                        SDL_SetJoystickVirtualButton(test_pad, SDL_GAMEPAD_BUTTON_SOUTH, false);
                        return 0;
                    });
                });
                each("SHADPS4_TEST_VIRTUAL_TURN", [&](double seconds) {
                    const bool left = seconds < 0.0;
                    seconds = std::abs(seconds);
                    later(seconds, [](void*, SDL_TimerID, Uint32) -> Uint32 {
                        const bool done = SDL_SetJoystickVirtualButton(
                            test_pad, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, true);
                        LOG_INFO(Input, "Test gamepad: left shoulder button held for a turn: {}",
                                 done ? "yes" : SDL_GetError());
                        return 0;
                    });
                    later(seconds + 0.2, left ? [](void*, SDL_TimerID, Uint32) -> Uint32 {
                        SDL_SetJoystickVirtualAxis(test_pad, SDL_GAMEPAD_AXIS_RIGHTX, -32767);
                        return 0;
                    } : [](void*, SDL_TimerID, Uint32) -> Uint32 {
                        SDL_SetJoystickVirtualAxis(test_pad, SDL_GAMEPAD_AXIS_RIGHTX, 32767);
                        return 0;
                    });
                    later(seconds + 0.4, [](void*, SDL_TimerID, Uint32) -> Uint32 {
                        SDL_SetJoystickVirtualAxis(test_pad, SDL_GAMEPAD_AXIS_RIGHTX, 0);
                        return 0;
                    });
                    later(seconds + 0.6, [](void*, SDL_TimerID, Uint32) -> Uint32 {
                        SDL_SetJoystickVirtualButton(test_pad, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER,
                                                     false);
                        return 0;
                    });
                });
            }
        }
    }
#endif

#if defined(SDL_PLATFORM_WIN32)
    window_info.type = WindowSystemType::Windows;
    window_info.render_surface = SDL_GetPointerProperty(SDL_GetWindowProperties(window),
                                                        SDL_PROP_WINDOW_WIN32_HWND_POINTER, NULL);
#elif defined(SDL_PLATFORM_LINUX) || defined(__FreeBSD__)
    // SDL doesn't have a platform define for FreeBSD AAAAAAAAAA
    if (SDL_strcmp(SDL_GetCurrentVideoDriver(), "x11") == 0) {
        window_info.type = WindowSystemType::X11;
        window_info.display_connection = SDL_GetPointerProperty(
            SDL_GetWindowProperties(window), SDL_PROP_WINDOW_X11_DISPLAY_POINTER, NULL);
        window_info.render_surface = (void*)SDL_GetNumberProperty(
            SDL_GetWindowProperties(window), SDL_PROP_WINDOW_X11_WINDOW_NUMBER, 0);
    } else if (SDL_strcmp(SDL_GetCurrentVideoDriver(), "wayland") == 0) {
        window_info.type = WindowSystemType::Wayland;
        window_info.display_connection = SDL_GetPointerProperty(
            SDL_GetWindowProperties(window), SDL_PROP_WINDOW_WAYLAND_DISPLAY_POINTER, NULL);
        window_info.render_surface = SDL_GetPointerProperty(
            SDL_GetWindowProperties(window), SDL_PROP_WINDOW_WAYLAND_SURFACE_POINTER, NULL);
    }
#elif defined(SDL_PLATFORM_MACOS)
    window_info.type = WindowSystemType::Metal;
    window_info.render_surface = SDL_Metal_GetLayer(SDL_Metal_CreateView(window));
#endif
    if (headless) {
        window_info = {};
    }
    // input handler init-s
    Input::ControllerOutput::LinkJoystickAxes();
    Input::ParseInputConfig(std::string(Common::ElfInfo::Instance().GameSerial()));
#ifndef ENABLE_BACHATA_RUNTIME
    controllers.TryOpenSDLControllers();
#endif

    if (EmulatorSettings.IsBackgroundControllerInput()) {
        SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    }
}

WindowSDL::~WindowSDL() = default;

void WindowSDL::SetIcon(const std::filesystem::path& path) {
#ifdef ENABLE_BACHATA_RUNTIME
    // Desktop X11 icons become a megabyte-scale property request. The embedded Android X server
    // does not need window-manager metadata, and processing it blocks Vulkan WSI negotiation.
    return;
#endif
    if (!std::filesystem::exists(path)) {
        LOG_WARNING(Core, "Could not find icon file '{}', using default icon.",
                    fmt::UTF(path.u8string()));
        SetDefaultWindowIcon(window);
        return;
    }

    Common::FS::IOFile file{path, Common::FS::FileAccessMode::Read,
                            Common::FS::FileType::BinaryFile,
                            Common::FS::FileShareFlag::ShareReadWrite};
    if (!file.IsOpen()) {
        LOG_ERROR(Core, "Failed to open window icon file '{}'.", fmt::UTF(path.u8string()));
        SetDefaultWindowIcon(window);
        return;
    }

    const u64 fileSize = file.GetSize();
    std::vector<u8> buf(fileSize);
    const size_t bytesRead = file.ReadRaw<u8>(buf.data(), fileSize);
    file.Close();
    if (bytesRead < fileSize) {
        LOG_ERROR(Core, "Failed to read window icon file '{}'.", fmt::UTF(path.u8string()));
        SetDefaultWindowIcon(window);
        return;
    }

    SetWindowIcon(window, buf);
}

void WindowSDL::WaitEvent() {
    // Called on main thread
    SDL_Event event;

    if (!SDL_WaitEvent(&event)) {
        return;
    }

    if (Libraries::Mouse::PushSDLEvent(event)) {
        return;
    }

    if (ImGui::Core::ProcessEvent(&event)) {
        return;
    }

    switch (event.type) {
    case SDL_EVENT_WINDOW_RESIZED:
    case SDL_EVENT_WINDOW_MAXIMIZED:
    case SDL_EVENT_WINDOW_RESTORED:
        OnResize();
        break;
    case SDL_EVENT_WINDOW_MINIMIZED:
    case SDL_EVENT_WINDOW_EXPOSED:
        is_shown = event.type == SDL_EVENT_WINDOW_EXPOSED;
        OnResize();
        break;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP:
    case SDL_EVENT_MOUSE_WHEEL:
    case SDL_EVENT_MOUSE_WHEEL_OFF:
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP:
        OnKeyboardMouseInput(&event);
        break;
    case SDL_EVENT_GAMEPAD_ADDED:
    case SDL_EVENT_GAMEPAD_REMOVED:
#ifndef ENABLE_BACHATA_RUNTIME
        controllers.TryOpenSDLControllers();
#endif
        break;
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
    case SDL_EVENT_GAMEPAD_BUTTON_UP:
    case SDL_EVENT_GAMEPAD_AXIS_MOTION:
    case SDL_EVENT_GAMEPAD_TOUCHPAD_DOWN:
    case SDL_EVENT_GAMEPAD_TOUCHPAD_UP:
    case SDL_EVENT_GAMEPAD_TOUCHPAD_MOTION:
    case SDL_EVENT_GAMEPAD_SENSOR_UPDATE:
        OnGamepadEvent(&event);
        break;
    case SDL_EVENT_QUIT:
        is_open = false;
        break;
    case SDL_EVENT_QUIT_DIALOG:
        Overlay::ToggleQuitWindow();
        break;
    case SDL_EVENT_TOGGLE_FULLSCREEN: {
        if (SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN) {
            SDL_SetWindowFullscreen(window, 0);
        } else {
            SDL_SetWindowFullscreen(window, SDL_WINDOW_FULLSCREEN);
        }
        break;
    }
    case SDL_EVENT_TOGGLE_PAUSE:
        if (DebugState.IsGuestThreadsPaused()) {
            LOG_INFO(Frontend, "Game Resumed");
            DebugState.ResumeGuestThreads();
        } else {
            LOG_INFO(Frontend, "Game Paused");
            DebugState.PauseGuestThreads();
        }
        break;
    case SDL_EVENT_CHANGE_CONTROLLER:
        UNREACHABLE_MSG("todo");
        break;
    case SDL_EVENT_TOGGLE_SIMPLE_FPS:
        Overlay::ToggleSimpleFps();
        break;
    case SDL_EVENT_RELOAD_INPUTS:
        Input::ParseInputConfig(std::string(Common::ElfInfo::Instance().GameSerial()));
        break;
    case SDL_EVENT_MOUSE_TO_JOYSTICK:
        SDL_SetWindowRelativeMouseMode(this->GetSDLWindow(),
                                       Input::ToggleMouseModeTo(Input::MouseMode::Joystick));
        break;
    case SDL_EVENT_MOUSE_TO_GYRO:
        SDL_SetWindowRelativeMouseMode(this->GetSDLWindow(),
                                       Input::ToggleMouseModeTo(Input::MouseMode::Gyro));
        break;
    case SDL_EVENT_MOUSE_TO_TOUCHPAD:
        SDL_SetWindowRelativeMouseMode(this->GetSDLWindow(),
                                       Input::ToggleMouseModeTo(Input::MouseMode::Touchpad));
        SDL_SetWindowRelativeMouseMode(this->GetSDLWindow(), false);
        break;
    case SDL_EVENT_ADD_VIRTUAL_USER:
        for (int i = 0; i < 4; i++) {
            if (controllers[i]->user_id == -1) {
                auto u = UserManagement.GetUserByPlayerIndex(i + 1);
                if (!u) {
                    break;
                }
                controllers[i]->user_id = u->user_id;
                controllers[i]->ConnectController(controllers[i]->m_sdl_gamepad);
                UserManagement.LoginUser(u, i + 1);
                break;
            }
        }
        break;
    case SDL_EVENT_REMOVE_VIRTUAL_USER:
        LOG_INFO(Input, "Remove user");
        for (int i = 3; i >= 0; i--) {
            if (controllers[i]->user_id != -1) {
                UserManagement.LogoutUser(UserManagement.GetUserByID(controllers[i]->user_id));
                controllers[i]->DisconnectController();
                controllers[i]->user_id = -1;
                break;
            }
        }
        break;
    case SDL_EVENT_RDOC_CAPTURE:
        if (VideoCore::IsRenderDocLoaded()) {
            VideoCore::TriggerCapture();
        } else {
            VideoCore::RequestScreenshot(VideoCore::ScreenshotRequest::GameOnly);
        }
        break;
    case SDL_EVENT_SCREENSHOT_WITH_OVERLAYS:
        VideoCore::RequestScreenshot(VideoCore::ScreenshotRequest::WithOverlays);
        break;
    default:
        break;
    }
}

void WindowSDL::InitTimers() {
    for (int i = 0; i < 4; ++i) {
        SDL_AddTimer(4, &PollController, controllers[i]);
    }
    SDL_AddTimer(33, Input::MousePolling, (void*)controllers[0]);
}

void WindowSDL::RequestKeyboard() {
    if (keyboard_grab == 0) {
        SDL_RunOnMainThread(
            [](void* userdata) { SDL_StartTextInput(static_cast<SDL_Window*>(userdata)); }, window,
            true);
    }
    keyboard_grab++;
}

void WindowSDL::ReleaseKeyboard() {
    ASSERT(keyboard_grab > 0);
    keyboard_grab--;
    if (keyboard_grab == 0) {
        SDL_RunOnMainThread(
            [](void* userdata) { SDL_StopTextInput(static_cast<SDL_Window*>(userdata)); }, window,
            true);
    }
}

void WindowSDL::OnResize() {
    SDL_GetWindowSizeInPixels(window, &width, &height);
    ImGui::Core::OnResize();
}

Uint32 wheelOffCallback(void* og_event, Uint32 timer_id, Uint32 interval) {
    SDL_Event off_event = *(SDL_Event*)og_event;
    off_event.type = SDL_EVENT_MOUSE_WHEEL_OFF;
    SDL_PushEvent(&off_event);
    delete (SDL_Event*)og_event;
    return 0;
}

void WindowSDL::OnKeyboardMouseInput(const SDL_Event* event) {
    using Libraries::Pad::OrbisPadButtonDataOffset;

    // get the event's id, if it's keyup or keydown
    const bool input_down = event->type == SDL_EVENT_KEY_DOWN ||
                            event->type == SDL_EVENT_MOUSE_BUTTON_DOWN ||
                            event->type == SDL_EVENT_MOUSE_WHEEL;
    Input::InputEvent input_event = Input::InputBinding::GetInputEventFromSDLEvent(*event);

    // if it's a wheel event, make a timer that turns it off after a set time
    if (event->type == SDL_EVENT_MOUSE_WHEEL) {
        const SDL_Event* copy = new SDL_Event(*event);
        SDL_AddTimer(33, wheelOffCallback, (void*)copy);
    }

    // add/remove it from the list
    bool inputs_changed = Input::UpdatePressedKeys(input_event);

    // update bindings
    if (inputs_changed) {
        Input::ActivateOutputsFromInputs();
    }
}

void WindowSDL::OnGamepadEvent(const SDL_Event* event) {
    bool input_down = event->type == SDL_EVENT_GAMEPAD_AXIS_MOTION ||
                      event->type == SDL_EVENT_GAMEPAD_BUTTON_DOWN;
    Input::InputEvent input_event = Input::InputBinding::GetInputEventFromSDLEvent(*event);

    // In a headset, the first player's gamepad and the headset's own controllers take turns:
    // whichever was used last plays (see Input::PadSource). A button pressed, a stick or a
    // trigger pushed, the touchpad touched is the gamepad being used.
    {
        SDL_JoystickID which = 0;
        bool used = false;
        switch (event->type) {
        case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
            which = event->gbutton.which;
            used = true;
            break;
        case SDL_EVENT_GAMEPAD_AXIS_MOTION:
            which = event->gaxis.which;
            used = std::abs(static_cast<int>(event->gaxis.value)) > 16000;
            break;
        case SDL_EVENT_GAMEPAD_TOUCHPAD_DOWN:
            which = event->gtouchpad.which;
            used = true;
            break;
        default:
            break;
        }
        auto* const first = controllers[0];
        if (used && first->m_sdl_gamepad != nullptr &&
            SDL_GetGamepadFromID(which) == first->m_sdl_gamepad) {
            const double now = std::chrono::duration<double>(
                                   std::chrono::steady_clock::now().time_since_epoch())
                                   .count();
            if (Input::FirstPadSource().GamepadUsed(now)) {
                first->SetHeadsetPlays(false);
                LOG_INFO(Input, "Controller 1 was used: it plays again, and the headset's "
                                "controllers are left aside until they are used");
            }
        }
    }

    // The PS button is nothing a title ever sees. In a headset, pressed and let go, it resets
    // the view. Held, it has the D-pad move the place a controller that nothing locates is
    // held to be at (up, down, left, right; L1 nearer, R1 farther; two centimetres a press),
    // triangle switch between that place and the standard one, and square blow into the
    // microphone for as long as it is held. None of that reaches the title.
    if (event->type == SDL_EVENT_GAMEPAD_BUTTON_DOWN ||
        event->type == SDL_EVENT_GAMEPAD_BUTTON_UP) {
        static bool held = false;
        static bool used = false;
        static u32 kept_back = 0;
        const bool down = event->type == SDL_EVENT_GAMEPAD_BUTTON_DOWN;
        const u8 button = event->gbutton.button;
        auto& runtime = Core::Vr::Runtime::Instance();
        if (button == SDL_GAMEPAD_BUTTON_GUIDE) {
            if (down) {
                held = true;
                used = false;
            } else {
                if (held && !used) {
                    runtime.NotePadButton(Core::Vr::Runtime::PadButton::Home, true);
                    runtime.NotePadButton(Core::Vr::Runtime::PadButton::Home, false);
                }
                held = false;
            }
        } else if (button < 32 && !down && (kept_back & (1u << button)) != 0) {
            // Let go of after it did something else than the title would have seen.
            kept_back &= ~(1u << button);
            if (button == SDL_GAMEPAD_BUTTON_WEST) {
                Input::SetBlowing(false);
            }
            return;
        } else if (held && down && button < 32) {
            static constexpr float Step = 0.02f;
            bool taken = true;
            switch (button) {
            case SDL_GAMEPAD_BUTTON_DPAD_UP:
                runtime.MoveOwnPadPlace({0.0f, Step, 0.0f});
                break;
            case SDL_GAMEPAD_BUTTON_DPAD_DOWN:
                runtime.MoveOwnPadPlace({0.0f, -Step, 0.0f});
                break;
            case SDL_GAMEPAD_BUTTON_DPAD_LEFT:
                runtime.MoveOwnPadPlace({-Step, 0.0f, 0.0f});
                break;
            case SDL_GAMEPAD_BUTTON_DPAD_RIGHT:
                runtime.MoveOwnPadPlace({Step, 0.0f, 0.0f});
                break;
            case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER:
                runtime.MoveOwnPadPlace({0.0f, 0.0f, Step});
                break;
            case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER:
                runtime.MoveOwnPadPlace({0.0f, 0.0f, -Step});
                break;
            case SDL_GAMEPAD_BUTTON_NORTH:
                runtime.SwitchPadPlace();
                break;
            case SDL_GAMEPAD_BUTTON_WEST:
                Input::SetBlowing(true);
                break;
            default:
                taken = false;
                break;
            }
            if (taken) {
                used = true;
                kept_back |= 1u << button;
                return;
            }
        }
    }

    // the touchpad button shouldn't be rebound to anything else,
    // as it would break the entire touchpad handling
    // You can still bind other things to it though
    if (event->gbutton.button == SDL_GAMEPAD_BUTTON_TOUCHPAD) {
        controllers[controllers.GetGamepadIndexFromJoystickId(event->gbutton.which)]->Button(
            OrbisPadButtonDataOffset::TouchPad, input_down);
        return;
    }

    u8 gamepad;

    switch (event->type) {
    case SDL_EVENT_GAMEPAD_SENSOR_UPDATE:
        switch ((SDL_SensorType)event->gsensor.sensor) {
        case SDL_SENSOR_GYRO:
            gamepad = controllers.GetGamepadIndexFromJoystickId(event->gsensor.which);
            if (gamepad < 5) {
                controllers[gamepad]->UpdateGyro(event->gsensor.data);
            }
            if (gamepad == 0) {
                const float* gyro = event->gsensor.data;
                const float speed = std::sqrt(gyro[0] * gyro[0] + gyro[1] * gyro[1] + gyro[2] * gyro[2]);
                if (Common::ElfInfo::Instance().GameSerial() != "CUSA12392" &&
                    Input::FirstPadSource().GamepadMotionUsed(speed)) {
                    Core::Vr::Runtime::Instance().ReleasePad();
                    LOG_INFO(Input, "Physical gamepad motion active: using its gyro and virtual position. "
                                    "Use a VR controller button to restore tracked motion.");
                }
                Core::Vr::Runtime::Instance().UpdatePadGyro({gyro[0], gyro[1], gyro[2]});
            }
            break;
        case SDL_SENSOR_ACCEL:
            gamepad = controllers.GetGamepadIndexFromJoystickId(event->gsensor.which);
            if (gamepad < 5) {
                controllers[gamepad]->UpdateAcceleration(event->gsensor.data);
            }
            if (gamepad == 0) {
                const float* accel = event->gsensor.data;
                Core::Vr::Runtime::Instance().UpdatePadAcceleration({accel[0], accel[1], accel[2]});
            }
            break;
        default:
            break;
        }
        return;
    case SDL_EVENT_GAMEPAD_TOUCHPAD_DOWN:
    case SDL_EVENT_GAMEPAD_TOUCHPAD_UP:
    case SDL_EVENT_GAMEPAD_TOUCHPAD_MOTION:
        controllers[controllers.GetGamepadIndexFromJoystickId(event->gtouchpad.which)]
            ->SetTouchpadState(event->gtouchpad.finger,
                               event->type != SDL_EVENT_GAMEPAD_TOUCHPAD_UP, event->gtouchpad.x,
                               event->gtouchpad.y);
        return;
    default:
        break;
    }

    // add/remove it from the list
    bool inputs_changed = Input::UpdatePressedKeys(input_event);

    if (inputs_changed) {
        // update bindings
        Input::ActivateOutputsFromInputs();
    }
}

#ifndef __APPLE__
void SetWindowIcon(SDL_Window* window, const std::vector<u8>& png) {
    int imageWidth = 0;
    int imageHeight = 0;
    constexpr int numChannels = 4;
    unsigned char* imageData = stbi_load_from_memory(png.data(), png.size(), &imageWidth,
                                                     &imageHeight, nullptr, numChannels);
    if (imageData == nullptr) {
        LOG_ERROR(Core, "Failed to load window icon image: {}", stbi_failure_reason());
        return;
    }
    SCOPE_EXIT {
        stbi_image_free(imageData);
    };

    SDL_Surface* surface = SDL_CreateSurfaceFrom(imageWidth, imageHeight, SDL_PIXELFORMAT_RGBA32,
                                                 imageData, imageWidth * numChannels);
    if (surface == nullptr) {
        LOG_ERROR(Core, "Failed to create SDL surface for window icon: {}", SDL_GetError());
    }
    if (!SDL_SetWindowIcon(window, surface)) {
        LOG_ERROR(Core, "Failed to set SDL window icon: {}", SDL_GetError());
    }
    SDL_DestroySurface(surface);
}
#endif

void SetDefaultWindowIcon(SDL_Window* window) {
    const auto resource = cmrc::res::get_filesystem();
    const auto file = resource.open("src/resources/shadps4.png");
    const std::vector<u8> texData = std::vector<u8>(file.begin(), file.end());
    SetWindowIcon(window, texData);
}

} // namespace Frontend
