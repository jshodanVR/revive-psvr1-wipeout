// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <filesystem>

namespace Input {

/// Replays controller input from a text file onto the first controller, for unattended test
/// runs. Each line is "<start seconds> <hold seconds> <input>..." where an input is a button
/// name (cross, circle, square, triangle, up, down, left, right, l1, r1, l2, r2, l3, r3,
/// options, touchpad) or an axis assignment (lx=, ly=, rx=, ry= in 0..255, 128 is centred).
/// For VR titles a line may also carry "head=x,y,z[,yaw,pitch,roll]" and
/// "pad=x,y,z[,yaw,pitch,roll]" (metres and degrees, relative to the resting head position) or
/// "padrot=yaw,pitch,roll"; those poses stay in effect until a later line replaces them.
/// "hands=x,y,z[,heading]" stands for a host that sees where the controller is held but not how
/// it is turned ("hands=off" for losing sight of it), "touch=x,y" (0..1) for a finger on the
/// touchpad while the line lasts, "finger=x,y" (-1..1) for the right stick as it moves that
/// finger, "gesture=press", "gesture=swipe" or "gesture=pull" for the buttons that do the
/// touchpad's gestures on a controller without one, held while the line lasts,
/// "mic=level" for noise of that loudness in the microphone
/// (root mean square, 1 is full scale: 0.35 is what a title takes for blowing hard),
/// "recenter" for the player asking for the view to be reset where the head is at that moment,
/// "recenter-pad" to also centre and level the tracked virtual pad (Statik's VR chord),
/// "worn=0" / "worn=1" for the headset being taken off and put back on.
/// Lines starting with '#' are ignored. Times count from when the script is started.
void StartScriptedInput(const std::filesystem::path& script);

/// The loudness a running script wants the microphone to hear right now, 0 for none. Also
/// what a title takes for blowing hard while the player holds the buttons that stand for it
/// (SetBlowing).
float ScriptedMicrophoneLevel();

/// The player blows without a microphone: with buttons (the PS button and square on a gamepad,
/// X and Y together on a headset's controllers), for where the microphone is not to be had,
/// gives too little, or blowing is not what the player wants to do.
void SetBlowing(bool blowing);

} // namespace Input
