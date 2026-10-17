// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <numbers>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "common/logging/log.h"
#include "common/singleton.h"
#include "common/thread.h"
#include "core/vr/vr_runtime.h"
#include "input/controller.h"
#include "input/scripted_input.h"
#include "input/pad_gestures.h"
#include "input/stick_finger.h"

namespace Input {

namespace {

using Buttons = Libraries::Pad::OrbisPadButtonDataOffset;

std::atomic<float> microphone_level{0.0f};
std::atomic<bool> blowing{false};

struct Step {
    double start;
    double end;
    Buttons buttons{Buttons::None};
    // Left x, left y, right x, right y; negative means "leave centred".
    std::array<int, 4> sticks{-1, -1, -1, -1};
    std::optional<Core::Vr::Pose> head;
    std::optional<Core::Vr::Pose> pad;
    std::array<std::optional<Core::Vr::Pose>, 2> moves;
    std::array<bool, 2> moves_off{};
    std::array<u16, 2> move_buttons{};
    std::array<u16, 2> move_triggers{};
    std::optional<Core::Vr::Quat> pad_rotation;
    // Where a host sees the hands hold the controller: x, y, z and heading in degrees.
    std::optional<std::array<float, 4>> hands;
    bool hands_lost{};
    // A finger on the touchpad, 0..1 across and down.
    std::optional<std::array<float, 2>> touch;
    // The right stick as it stands in for that finger (Input::StickFinger), -1..1 to the right
    // and towards the player.
    std::optional<std::array<float, 2>> finger;
    // The buttons that do the touchpad's gestures on a controller without one
    // (Input::PadGestures), held while the line lasts.
    PadGestures::Controls gestures;
    // The place a controller that nothing locates is held to be at: moved by so many metres
    // (right, up, towards the player), or switched between the standard one and the player's.
    std::optional<Core::Vr::Vec3> place_move;
    bool place_switch{};
    // What the microphone hears: noise of this loudness (root mean square, 1 is full scale).
    float microphone{};
    bool recenter{};
    bool neutral_pad{};
    // The headset taken off the head (false) or put back on (true), from this line on.
    std::optional<bool> worn;
};

std::vector<float> ParseNumbers(const std::string& text) {
    std::vector<float> numbers;
    std::istringstream stream{text};
    std::string item;
    while (std::getline(stream, item, ',')) {
        numbers.push_back(std::stof(item));
    }
    return numbers;
}

Core::Vr::Quat ParseRotation(const std::vector<float>& numbers, size_t first) {
    static constexpr float Radians = std::numbers::pi_v<float> / 180.0f;
    const auto angle = [&](size_t index) {
        return first + index < numbers.size() ? numbers[first + index] * Radians : 0.0f;
    };
    return Core::Vr::FromYawPitchRoll(angle(0), angle(1), angle(2));
}

std::optional<Core::Vr::Pose> ParsePose(const std::string& text) {
    const auto numbers = ParseNumbers(text);
    if (numbers.size() < 3) {
        return std::nullopt;
    }
    Core::Vr::Pose pose;
    pose.position = {numbers[0], numbers[1], numbers[2]};
    pose.orientation = ParseRotation(numbers, 3);
    return pose;
}

const std::unordered_map<std::string, Buttons> ButtonNames = {
    {"cross", Buttons::Cross},       {"circle", Buttons::Circle}, {"square", Buttons::Square},
    {"triangle", Buttons::Triangle}, {"up", Buttons::Up},         {"down", Buttons::Down},
    {"left", Buttons::Left},         {"right", Buttons::Right},   {"l1", Buttons::L1},
    {"r1", Buttons::R1},             {"l2", Buttons::L2},         {"r2", Buttons::R2},
    {"l3", Buttons::L3},             {"r3", Buttons::R3},         {"options", Buttons::Options},
    {"touchpad", Buttons::TouchPad},
};

std::vector<Step> ParseScript(const std::filesystem::path& script) {
    std::vector<Step> steps;
    std::ifstream file{script};
    std::string line;
    while (std::getline(file, line)) {
        if (line.empty() || line[0] == '#') {
            continue;
        }
        std::istringstream tokens{line};
        double start = 0.0;
        double hold = 0.0;
        if (!(tokens >> start >> hold)) {
            continue;
        }
        Step step{.start = start, .end = start + hold};
        std::string token;
        while (tokens >> token) {
            if (const auto it = ButtonNames.find(token); it != ButtonNames.end()) {
                step.buttons |= it->second;
                continue;
            }
            if (token.starts_with("head=")) {
                step.head = ParsePose(token.substr(5));
                continue;
            }
            if (token.starts_with("pad=")) {
                step.pad = ParsePose(token.substr(4));
                continue;
            }
            bool move_token = false;
            for (u32 hand = 0; hand < 2; ++hand) {
                const std::string prefix = hand == 0 ? "move_left" : "move_right";
                if (token.starts_with(prefix + "=")) {
                    const auto value = token.substr(prefix.size() + 1);
                    step.moves_off[hand] = value == "off";
                    if (!step.moves_off[hand]) step.moves[hand] = ParsePose(value);
                    move_token = true;
                } else if (token.starts_with(prefix + "_buttons=")) {
                    step.move_buttons[hand] = static_cast<u16>(std::stoul(token.substr(prefix.size() + 9), nullptr, 0));
                    move_token = true;
                } else if (token.starts_with(prefix + "_trigger=")) {
                    step.move_triggers[hand] = static_cast<u16>(std::clamp(std::stoi(token.substr(prefix.size() + 9)), 0, 255));
                    move_token = true;
                }
            }
            if (move_token) continue;
            if (token.starts_with("padrot=")) {
                step.pad_rotation = ParseRotation(ParseNumbers(token.substr(7)), 0);
                continue;
            }
            if (token == "hands=off") {
                step.hands_lost = true;
                continue;
            }
            if (token.starts_with("hands=")) {
                const auto numbers = ParseNumbers(token.substr(6));
                if (numbers.size() >= 3) {
                    step.hands = std::array<float, 4>{numbers[0], numbers[1], numbers[2],
                                                      numbers.size() > 3 ? numbers[3] : 0.0f};
                }
                continue;
            }
            if (token.starts_with("mic=")) {
                step.microphone = std::stof(token.substr(4));
                continue;
            }
            if (token == "recenter" || token == "recenter-pad") {
                step.recenter = true;
                step.neutral_pad = token == "recenter-pad";
                continue;
            }
            if (token == "worn=0" || token == "worn=1") {
                step.worn = token.back() == '1';
                continue;
            }
            if (token.starts_with("touch=")) {
                const auto numbers = ParseNumbers(token.substr(6));
                if (numbers.size() >= 2) {
                    step.touch = std::array<float, 2>{numbers[0], numbers[1]};
                }
                continue;
            }
            if (token.starts_with("finger=")) {
                const auto numbers = ParseNumbers(token.substr(7));
                if (numbers.size() >= 2) {
                    step.finger = std::array<float, 2>{numbers[0], numbers[1]};
                }
                continue;
            }
            if (token == "gesture=press" || token == "gesture=swipe" || token == "gesture=pull") {
                step.gestures.press = step.gestures.press || token == "gesture=press";
                step.gestures.swipe = step.gestures.swipe || token == "gesture=swipe";
                step.gestures.pull = step.gestures.pull || token == "gesture=pull";
                continue;
            }
            if (token == "place=switch") {
                step.place_switch = true;
                continue;
            }
            if (token.starts_with("place=")) {
                const auto numbers = ParseNumbers(token.substr(6));
                if (numbers.size() >= 3) {
                    step.place_move = Core::Vr::Vec3{numbers[0], numbers[1], numbers[2]};
                }
                continue;
            }
            static constexpr std::array<std::string_view, 4> AxisNames = {"lx=", "ly=", "rx=",
                                                                           "ry="};
            for (size_t axis = 0; axis < AxisNames.size(); ++axis) {
                if (token.starts_with(AxisNames[axis])) {
                    step.sticks[axis] = std::stoi(token.substr(AxisNames[axis].size()));
                }
            }
        }
        steps.push_back(step);
    }
    return steps;
}

void Replay(std::vector<Step> steps, std::filesystem::path live_script) {
    Common::SetCurrentThreadName("shadPS4:ScriptedInput");
    const auto begin = std::chrono::steady_clock::now();
    double last_end = 0.0;
    for (const Step& step : steps) {
        last_end = std::max(last_end, step.end);
    }

    // The script's poses are counted from where the player sits.
    Core::Vr::Runtime::Instance().FixSeat();
    std::vector<bool> started(steps.size());

    Buttons previous_buttons{Buttons::None};
    std::array<int, 6> previous_axes{128, 128, 128, 128, 0, 0};
    std::optional<std::array<float, 2>> previous_touch;
    const char* inbox_env = std::getenv("SHADPS4_INPUT_INBOX");
    const std::filesystem::path inbox = inbox_env ? inbox_env : "";
    double next_inbox_check = 0.0;
    StickFinger stick_finger;
    PadGestures gestures;
    std::filesystem::file_time_type live_stamp{};
    while (true) {
        if (!live_script.empty()) {
            std::error_code error;
            const auto stamp = std::filesystem::last_write_time(live_script, error);
            if (!error && stamp != live_stamp) {
                live_stamp = stamp;
                steps = ParseScript(live_script);
                started.resize(steps.size());
                LOG_INFO(Input, "Live script now has {} steps", steps.size());
            }
        }
        const double now =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - begin).count();

        // Opt-in local research control: a one-shot script, relative to its receipt.
        // Producers should write a temporary file then rename it to this inbox.
        // Normal headset launches never set this path.
        if (!inbox.empty() && now >= next_inbox_check) {
            next_inbox_check = now + 0.25;
            std::error_code ec;
            if (std::filesystem::exists(inbox, ec)) {
                try {
                    auto extra = ParseScript(inbox);
                    std::filesystem::remove(inbox, ec);
                    const bool valid = !extra.empty() && extra.size() <= 256 &&
                        steps.size() + extra.size() <= 8192 &&
                        std::all_of(extra.begin(), extra.end(), [](const Step& step) {
                            return std::isfinite(step.start) && std::isfinite(step.end) &&
                                   step.start >= 0 && step.end >= step.start && step.end <= 1800;
                        });
                    if (valid) {
                        for (auto& step : extra) {
                            step.start += now;
                            step.end += now;
                            last_end = std::max(last_end, step.end);
                            steps.push_back(std::move(step));
                        }
                        started.resize(steps.size());
                        LOG_INFO(Input, "Accepted {} live research input steps at {:.2f}s", extra.size(), now);
                    } else {
                        LOG_WARNING(Input, "Rejected empty, excessive or invalid live input script");
                    }
                } catch (const std::exception& error) {
                    std::filesystem::remove(inbox, ec);
                    LOG_ERROR(Input, "Live research input rejected: {}", error.what());
                }
            }
        }

        Buttons buttons{Buttons::None};
        std::array<int, 6> axes{128, 128, 128, 128, 0, 0};
        const Step* head_step = nullptr;
        const Step* pad_step = nullptr;
        std::array<const Step*, 2> move_steps{};
        std::array<u16, 2> move_buttons{};
        std::array<u16, 2> move_triggers{};
        const Step* hands_step = nullptr;
        std::optional<std::array<float, 2>> touch;
        std::array<float, 2> finger{};
        PadGestures::Controls controls;
        float microphone = 0.0f;
        for (const Step& step : steps) {
            if (now >= step.start) {
                for (u32 hand = 0; hand < 2; ++hand) {
                    if ((step.moves[hand] || step.moves_off[hand]) &&
                        (!move_steps[hand] || step.start >= move_steps[hand]->start))
                        move_steps[hand] = &step;
                }
                // Poses persist, the most recently started line wins.
                if (step.head && (head_step == nullptr || step.start >= head_step->start)) {
                    head_step = &step;
                }
                if ((step.pad || step.pad_rotation) &&
                    (pad_step == nullptr || step.start >= pad_step->start)) {
                    pad_step = &step;
                }
                if ((step.hands || step.hands_lost) &&
                    (hands_step == nullptr || step.start >= hands_step->start)) {
                    hands_step = &step;
                }
            }
            if (now < step.start || now >= step.end) {
                continue;
            }
            if (step.touch) {
                touch = step.touch;
            }
            if (step.finger) {
                finger = *step.finger;
            }
            controls.press = controls.press || step.gestures.press;
            controls.swipe = controls.swipe || step.gestures.swipe;
            controls.pull = controls.pull || step.gestures.pull;
            microphone = std::max(microphone, step.microphone);
            for (u32 hand = 0; hand < 2; ++hand) {
                move_buttons[hand] |= step.move_buttons[hand];
                move_triggers[hand] = std::max(move_triggers[hand], step.move_triggers[hand]);
            }
            buttons |= step.buttons;
            for (size_t axis = 0; axis < step.sticks.size(); ++axis) {
                if (step.sticks[axis] >= 0) {
                    axes[axis] = step.sticks[axis];
                }
            }
        }
        microphone_level.store(microphone, std::memory_order_relaxed);
        // What buttons do on the touchpad comes before what the stick does there.
        if (const auto made = gestures.Update(now, controls); made.down && !touch) {
            touch = std::array<float, 2>{made.x, made.y};
            if (made.pressed) {
                buttons |= Buttons::TouchPad;
            }
            stick_finger.Reset();
        } else if (const auto stick = stick_finger.Update(now, finger[0], finger[1]);
                   stick.down && !touch) {
            // (A stick at rest between its lines: that is what lifts its finger.)
            touch = std::array<float, 2>{stick.x, stick.y};
        }
        auto& vr = Core::Vr::Runtime::Instance();
        std::array<Core::Vr::DeviceState, 2> scripted_hands{};
        for (u32 hand = 0; hand < 2; ++hand) {
            if (const auto* step = move_steps[hand]) {
                const bool connected = !step->moves_off[hand];
                Core::Vr::DeviceState state{};
                if (connected && step->moves[hand]) {
                    state.pose = *step->moves[hand];
                    state.tracked = true;
                }
                const u16 trigger_bit = move_triggers[hand] > 30 ? 2 : 0;
                vr.UpdateMove(hand, state, move_buttons[hand] | trigger_bit, move_triggers[hand], connected);
                scripted_hands[hand] = state;
            }
        }
        if (head_step != nullptr) {
            vr.UpdateHead({.pose = *head_step->head, .tracked = true});
        }
        if (vr.AimEnabled()) {
            // Reuse scripted hand poses, not Move devices: 0=left/support, 1=right/rear.
            vr.UpdateAim(scripted_hands[1], scripted_hands[0], True(buttons & Buttons::L1));
        }
        for (size_t i = 0; i < steps.size(); ++i) {
            if (now < steps[i].start || started[i]) {
                continue;
            }
            // Things that happen once, when their line starts.
            started[i] = true;
            if (steps[i].recenter) {
                // The player asks for the view to be reset, wherever the head is right now.
                vr.RequestRecenter(steps[i].neutral_pad);
            }
            if (steps[i].place_move) {
                vr.MoveOwnPadPlace(*steps[i].place_move);
            }
            if (steps[i].place_switch) {
                vr.SwitchPadPlace();
            }
            if (steps[i].worn) {
                LOG_INFO(Input, "Scripted: the headset is {}", *steps[i].worn ? "put on"
                                                                              : "taken off");
                vr.SetHeadsetWorn(*steps[i].worn);
            }
        }
        if (pad_step != nullptr) {
            if (pad_step->pad) {
                vr.UpdatePad({.pose = *pad_step->pad, .tracked = true});
            } else {
                vr.UpdatePadOrientation(*pad_step->pad_rotation, {});
            }
        }

        if (hands_step != nullptr) {
            if (hands_step->hands) {
                const auto& hands = *hands_step->hands;
                vr.UpdatePadPosition({hands[0], hands[1], hands[2]}, {});
                vr.UpdatePadYawReference(hands[3] * std::numbers::pi_v<float> / 180.0f);
            } else {
                vr.ClearPadPosition();
            }
        }

        // The analog triggers follow their digital buttons.
        axes[4] = True(buttons & Buttons::L2) ? 255 : 0;
        axes[5] = True(buttons & Buttons::R2) ? 255 : 0;

        if (buttons != previous_buttons || axes != previous_axes || touch != previous_touch) {
            auto* controllers = Common::Singleton<GameControllers>::Instance();
            (*controllers)[0]->ApplyRemoteState(buttons, axes, touch.has_value(),
                                                touch ? (*touch)[0] : 0.0f,
                                                touch ? (*touch)[1] : 0.0f);
            previous_buttons = buttons;
            previous_axes = axes;
            previous_touch = touch;
        }
        if (live_script.empty() && now > last_end + 1.0) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    microphone_level.store(0.0f, std::memory_order_relaxed);
    Core::Vr::Runtime::Instance().ReleaseMoves();
    Core::Vr::Runtime::Instance().ReleaseAim();
    LOG_INFO(Input, "Scripted input finished");
}

} // namespace

float ScriptedMicrophoneLevel() {
    // (0.36: a little above what ASTRO BOT takes for blowing as hard as can be.)
    return std::max(microphone_level.load(std::memory_order_relaxed),
                    blowing.load(std::memory_order_relaxed) ? 0.36f : 0.0f);
}

void SetBlowing(bool blowing_now) {
    if (blowing.exchange(blowing_now, std::memory_order_relaxed) != blowing_now && blowing_now) {
        static std::atomic<bool> said{false};
        if (!said.exchange(true)) {
            LOG_INFO(Input,
                     "Blowing with buttons: the game hears it as blowing into the microphone");
        }
    }
}

void StartScriptedInput(const std::filesystem::path& script) {
    auto steps = ParseScript(script);
    if (steps.empty()) {
        LOG_ERROR(Input, "No usable steps in input script {}", script.string());
        return;
    }
    LOG_INFO(Input, "Replaying {} input steps from {}", steps.size(), script.string());
    const char* live = std::getenv("SHADPS4_LIVE_INPUT");
    std::thread{Replay, std::move(steps), std::filesystem::path{live ? live : ""}}.detach();
}

} // namespace Input
