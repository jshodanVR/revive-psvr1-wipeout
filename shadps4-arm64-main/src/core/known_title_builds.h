// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <cstring>
#include <span>
#include <vector>

#include "common/types.h"

/// Where things are in the builds of ASTRO BOT Rescue Mission (CUSA12392) that are known from
/// inside, and how a build is told from every other. Kept apart from what is done with it
/// (known_title.cpp) so that it can be tried without an emulator around it
/// (tools/tests/known_title_builds_test.cpp).
///
/// The game's updates moved its code and data about, each part by an amount of its own: every
/// address below was found in the build it is given for, none is worked out from another's.
/// (The update 1.04 offsets were documented and tested in upstream AstroQuest issue #1.)
namespace Core::KnownTitle::Builds {

constexpr s32 FirstHeadsetLevel = 3;
constexpr s32 LastHeadsetLevel = 6;
// The sizes the title draws its scene at, an eye, as the console has them: the first three are
// for a television, the others for the headset.
constexpr std::array<std::array<u32, 2>, 7> ConsoleSizes{{
    {640, 360}, {1280, 720}, {1920, 1080}, {816, 870}, {960, 1080}, {1200, 1280}, {1440, 1536}}};
// What the scene's targets are taken from: a pool of 200 MB, a smaller one of 10 MB for what
// goes with them, and the heap of graphics memory both come from, 872 MB, which the title takes
// from the console's memory at its start.
constexpr u32 ConsoleTargetPool = 0xc800000;
constexpr u32 ConsoleSmallPool = 0xa00000;
constexpr u64 ConsoleGraphicsHeap = 0x36800000;
// The time one frame stands for, as the title's engine has it before the title runs.
constexpr double ConsoleFrameRate = 60.0;
constexpr float ConsoleFrameSeconds = 1.0f / 60.0f;
constexpr u64 ConsoleFrameMicroseconds = 16666;

// The function of the title's tracking manager that asks for the head's position to be taken
// anew: it reads the same in every build, and is somewhere else in each.
constexpr std::array<u8, 9> SetRecentreCode{0x40, 0x0f, 0xb6, 0xc6, 0xff, 0xc0, 0x89, 0x07, 0xc3};

/// One place of the title's image that is to hold something else: what the console's build has
/// there and what it is to have, in so many bytes.
struct Change {
    u64 at;
    u64 was;
    u64 now;
    u32 bytes;
};

/// The title's world of rigid bodies (Sony's PhysicsEffects under a class of its own), as far
/// as the emulator looks into it. The same in every build known.
namespace Physics {
// From the game's main object to the bodies: the world, what it wraps, the library's world.
constexpr u64 GameWorld = 0x10;
constexpr u64 WorldInner = 0x8;
constexpr u64 InnerScale = 0x58; // float: the library's lengths for one of the game's
constexpr u64 InnerLibrary = 0x20;
constexpr u64 LibraryBodyCount = 0x1a0; // u32
constexpr u64 LibraryBodies = 0x1a8;    // pointer
constexpr u64 LibraryTimeStep = 0x2760; // float
// One body's state.
constexpr u64 BodySize = 0xa0;
constexpr u64 BodyMotion = 0x1;    // u8
constexpr u64 BodySegment = 0x30;  // s32 [3]
constexpr u64 BodyPosition = 0x40; // float [3]
constexpr u64 BodyVelocity = 0x60; // float [3]
// A body that is moved by hand: sent to a place, which it reaches with the next step.
constexpr u8 MotionKeyframe = 2;
} // namespace Physics

struct Build {
    /// What the log calls it.
    const char* name;
    u64 set_recentre;
    /// The tracking manager, a singleton: where the pointer to it is kept.
    u64 manager_pointer;
    /// The engine's frame rate and what it derives from it when the rate is set: the time one
    /// frame stands for, in seconds and in microseconds. Everything in the game that moves
    /// reads one of the latter two.
    u64 frame_rate;         // double
    u64 frame_seconds;      // float
    u64 frame_microseconds; // u64
    /// What sets the size the scene is drawn at, a singleton made when first asked for: where
    /// the pointer to it is kept.
    u64 resolution_pointer;
    /// Where the sizes are written down: a table of widths, one of heights, the headset's once
    /// more in the switch of the function that makes the scene's targets (heights, then
    /// widths), and a table of pixel counts the title's own choice goes by.
    u64 size_widths;  // u32 [7]
    u64 size_heights; // u32 [7]
    std::array<std::array<u64, 2>, 4> size_switch;
    u64 size_pixels; // u64, 32 bytes apart
    /// The pictures handed to the headset, made three pairs at a time: their width and height.
    std::array<u64, 4> eye_sizes;
    /// The sizes of the two pools (in two places and in three) and of the heap.
    std::array<u64, 2> target_pool;
    std::array<u64, 3> small_pool;
    u64 graphics_heap; // u64
    /// The game's main object, a singleton: where the pointer to it is kept. It knows the
    /// world of rigid bodies (namespace Physics).
    u64 game_pointer;
    /// The end of the function that has that world take a step (PhysicsStepChanges); places
    /// of no bytes fill the list up.
    std::array<Change, 4> physics_step;
};

inline constexpr std::array<Build, 2> Known{{
    {
        .name = "1.00, as on the disc",
        .set_recentre = 0xc48320,
        .manager_pointer = 0x2e025a8,
        .frame_rate = 0x16688a8,
        .frame_seconds = 0x16688b0,
        .frame_microseconds = 0x16688b8,
        .resolution_pointer = 0x2dff9e0,
        .size_widths = 0x12da900,
        .size_heights = 0x12da920,
        .size_switch = {{{0xf2242f, 0xf22435},
                         {0xf2240d, 0xf22413},
                         {0xf22551, 0xf22557},
                         {0xf2255f, 0xf22565}}},
        .size_pixels = 0x1645048,
        .eye_sizes = {0xc3fad0, 0xc3fad5, 0xc3fb5c, 0xc3fb61},
        .target_pool = {0xef8bf7, 0xef8c4d},
        .small_pool = {0xf22194, 0xf221be, 0xf221dd},
        .graphics_heap = 0x1269708,
        .game_pointer = 0x2dff9b8,
        // mov rdi, [rdi+0x20]; vmovss [rdi+0x2760], xmm0; jmp <the library's step>; (padding)
        // becomes
        // mov rdi, [rdi+0x20]; vmovd eax, xmm0; push rax; push rdi; push rdi; call <step>;
        // pop rdi; pop rdi; pop rax; mov [rdi+0x2760], eax; ret
        .physics_step = {{{0xc2dea0, 0x8711fac5207f8b48, 0xc07ef9c5207f8b48, 8},
                          {0xc2dea8, 0x0449bfe900002760, 0x000449c0e8575750, 8},
                          {0xc2deb0, 0x9090909090909000, 0x0027608789585f5f, 8},
                          {0xc2deb8, 0x9090909090909090, 0x909090909090c300, 8}}},
    },
    {
        .name = "1.04, the last update",
        .set_recentre = 0xccc0b0,
        .manager_pointer = 0x2ed2ae8,
        .frame_rate = 0x17208b8,
        .frame_seconds = 0x17208c0,
        .frame_microseconds = 0x17208c8,
        .resolution_pointer = 0x2ecfe60,
        .size_widths = 0x1371120,
        .size_heights = 0x1371140,
        .size_switch = {{{0xfa5a1f, 0xfa5a25},
                         {0xfa59fd, 0xfa5a03},
                         {0xfa5b41, 0xfa5b47},
                         {0xfa5b4f, 0xfa5b55}}},
        .size_pixels = 0x16fb558,
        .eye_sizes = {0xcc3580, 0xcc3585, 0xcc360c, 0xcc3611},
        .target_pool = {0xf7c117, 0xf7c16d},
        .small_pool = {0xfa5784, 0xfa57ae, 0xfa57cd},
        .graphics_heap = 0x12ffb78,
        .game_pointer = 0x2ecfe38,
        // (mov rdi, [r15+0x20];) vmovss xmm0, [rbp-0x74]; vmovss [rdi+0x2760], xmm0;
        // call <the library's step>
        // becomes
        // (mov rdi, [r15+0x20];) call <step>; mov eax, [rbp-0x74]; mov rdi, [r15+0x20];
        // mov [rdi+0x2760], eax
        .physics_step = {{{0xcb09cf, 0x11fac58c4510fac5, 0x8c458b00045cfce8, 8},
                          {0xcb09d7, 0x5cefe80000276087, 0x27608789207f8b49, 8},
                          {0xcb09df, 0x0004, 0x0000, 2},
                          {0, 0, 0, 0}}},
    },
}};

/// The sizes of a title that draws larger than on the console, and the memory that takes.
struct Sizes {
    std::array<std::array<u32, 2>, 7> sizes{ConsoleSizes};
    u32 target_pool{ConsoleTargetPool};
    u32 small_pool{ConsoleSmallPool};
    u64 graphics_heap{ConsoleGraphicsHeap};
};

/// Every place that sets the sizes of a build, with what the console has there and what
/// `sizes` wants there.
inline std::vector<Change> SizeChanges(const Build& build, const Sizes& sizes) {
    std::vector<Change> changes;
    for (s32 level = FirstHeadsetLevel; level <= LastHeadsetLevel; ++level) {
        const auto& was = ConsoleSizes[level];
        const auto& now = sizes.sizes[level];
        changes.push_back({build.size_widths + 4 * level, was[0], now[0], 4});
        changes.push_back({build.size_heights + 4 * level, was[1], now[1], 4});
        changes.push_back({build.size_switch[level - FirstHeadsetLevel][0], was[1], now[1], 4});
        changes.push_back({build.size_switch[level - FirstHeadsetLevel][1], was[0], now[0], 4});
        changes.push_back(
            {build.size_pixels + 32 * level, u64{was[0]} * was[1], u64{now[0]} * now[1], 8});
    }
    const auto& eye_was = ConsoleSizes[LastHeadsetLevel];
    const auto& eye_now = sizes.sizes[LastHeadsetLevel];
    for (u32 i = 0; i < build.eye_sizes.size(); ++i) {
        changes.push_back({build.eye_sizes[i], eye_was[i % 2], eye_now[i % 2], 4});
    }
    for (const u64 at : build.target_pool) {
        changes.push_back({at, ConsoleTargetPool, sizes.target_pool, 4});
    }
    for (const u64 at : build.small_pool) {
        changes.push_back({at, ConsoleSmallPool, sizes.small_pool, 4});
    }
    changes.push_back({build.graphics_heap, ConsoleGraphicsHeap, sizes.graphics_heap, 8});
    return changes;
}

/// What makes the title's collisions land where they are sent when its time step is not the
/// same from one frame to the next.
///
/// The title moves the collision bodies of what moves by hand (platforms, doors, a part of a
/// level that rises) by sending them to a place: the library gives the body the speed that
/// takes it there in one step, speed = distance / time step, and the step that follows moves
/// it by speed x time step. The time step it divides by is the one of the step last taken; the
/// one the body is then moved by is the next frame's. On the console both are a sixtieth of a
/// second and the body arrives. With a time step that follows what frames take
/// (KnownTitle::OnFrameSubmitted) the body stops short or goes too far, by as much of its way
/// as the two steps differ. For what moves all the time that is put right a frame later, and
/// never seen. For what is sent once it stays: the parts of a level the title leaves alone
/// until the player gets near (its sections) are sent, all at once and in the one frame that
/// wakes them, from where they were to where the level has moved since. At the end of level
/// 2-1 that is 17 units up a tree that grew while they slept, and the frame that wakes them
/// is a long one: the mound's collisions ended most of a block above the mound (issue #16).
///
/// The change: the function that has the library take a step wrote down the new time step
/// and then took the step. It now takes the step with the time step that is written down, the
/// one every body was sent with, and writes the new one down after.
inline std::vector<Change> PhysicsStepChanges(const Build& build) {
    std::vector<Change> changes;
    for (const Change& change : build.physics_step) {
        if (change.bytes != 0) {
            changes.push_back(change);
        }
    }
    return changes;
}

/// The first of some places that does not hold what the console's build has there (or lies
/// outside the image), nullptr when all do.
inline const Change* FirstUnexpected(std::span<const u8> image, std::span<const Change> changes) {
    for (const Change& change : changes) {
        if (change.bytes > sizeof(u64) || change.at > image.size() ||
            image.size() - change.at < change.bytes) {
            return &change;
        }
        u64 found = 0;
        std::memcpy(&found, image.data() + change.at, change.bytes);
        if (found != change.was) {
            return &change;
        }
    }
    return nullptr;
}

/// Whether an image, as loaded and before any of it has run, is this build: the function that
/// tells builds apart is where it is in this one, the engine's time step is where this one
/// keeps it, and so is every size. (After the title has run, or once its sizes have been
/// changed, this is no longer true of the very same image: ask once.)
inline bool Is(const Build& build, std::span<const u8> image) {
    const auto holds = [&](u64 at, const void* bytes, size_t size) {
        return at <= image.size() && image.size() - at >= size &&
               std::memcmp(image.data() + at, bytes, size) == 0;
    };
    // (The two singletons are made while the title runs: their pointers only have to be
    // inside the image.)
    for (const u64 pointer : {build.manager_pointer, build.resolution_pointer}) {
        if (pointer > image.size() || image.size() - pointer < sizeof(u64)) {
            return false;
        }
    }
    if (!holds(build.set_recentre, SetRecentreCode.data(), SetRecentreCode.size()) ||
        !holds(build.frame_rate, &ConsoleFrameRate, sizeof(ConsoleFrameRate)) ||
        !holds(build.frame_seconds, &ConsoleFrameSeconds, sizeof(ConsoleFrameSeconds)) ||
        !holds(build.frame_microseconds, &ConsoleFrameMicroseconds,
               sizeof(ConsoleFrameMicroseconds))) {
        return false;
    }
    for (u32 level = 0; level < ConsoleSizes.size(); ++level) {
        if (!holds(build.size_widths + 4 * level, &ConsoleSizes[level][0], sizeof(u32)) ||
            !holds(build.size_heights + 4 * level, &ConsoleSizes[level][1], sizeof(u32))) {
            return false;
        }
    }
    // Every place a larger picture is written to, as the console has it.
    return FirstUnexpected(image, SizeChanges(build, Sizes{})) == nullptr;
}

/// The build an image is, as loaded and before any of it has run; nullptr for one that is none
/// of those known (or would be more than one of them, which no build of the title is).
inline const Build* Recognise(std::span<const u8> image) {
    const Build* found = nullptr;
    for (const Build& build : Known) {
        if (!Is(build, image)) {
            continue;
        }
        if (found != nullptr) {
            return nullptr;
        }
        found = &build;
    }
    return found;
}

/// Writes changes into an image, all of them or none: nothing is written unless every place
/// holds what the console's build has there. Answers with the place that did not, nullptr
/// when all was written.
inline const Change* Apply(std::span<u8> image, std::span<const Change> changes) {
    if (const Change* unexpected = FirstUnexpected(image, changes); unexpected != nullptr) {
        return unexpected;
    }
    for (const Change& change : changes) {
        std::memcpy(image.data() + change.at, &change.now, change.bytes);
    }
    return nullptr;
}

} // namespace Core::KnownTitle::Builds
