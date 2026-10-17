// SPDX-FileCopyrightText: Copyright 2020 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_master_semaphore.h"

#include "common/assert.h"
#include "common/logging/log.h"

namespace Vulkan {

namespace {

/// How long one call into the driver may wait. A wait that ends this way just looks again: the
/// limit is there so that nothing depends on the driver waking the thread up.
constexpr u64 WaitSliceNs = 100'000'000;

/// SHADPS4_TIMELINE_WAIT=direct asks the driver for the wanted tick itself, in one endless wait,
/// as upstream does: the way to see what the ordered waits below are for.
bool WaitsDirectly() {
    static const bool direct = [] {
        const char* value = std::getenv("SHADPS4_TIMELINE_WAIT");
        return value != nullptr && std::strcmp(value, "direct") == 0;
    }();
    return direct;
}

} // namespace

MasterSemaphore::MasterSemaphore(const Instance& instance_) : instance{instance_} {
    std::fprintf(stderr, "BACHATA_MASTER_SEMAPHORE_ENTER\n");
    const vk::StructureChain semaphore_chain = {
        vk::SemaphoreCreateInfo{},
        vk::SemaphoreTypeCreateInfo{
            .semaphoreType = vk::SemaphoreType::eTimeline,
            .initialValue = 0,
        },
    };
    auto [semaphore_result, sem] =
        instance.GetDevice().createSemaphoreUnique(semaphore_chain.get());
    ASSERT_MSG(semaphore_result == vk::Result::eSuccess, "Failed to create master semaphore: {}",
               vk::to_string(semaphore_result));
    semaphore = std::move(sem);
    std::fprintf(stderr, "BACHATA_MASTER_SEMAPHORE_READY\n");
}

MasterSemaphore::~MasterSemaphore() = default;

void MasterSemaphore::NoteReached(u64 tick) noexcept {
    u64 known = gpu_tick.load(std::memory_order_acquire);
    while (known < tick && !gpu_tick.compare_exchange_weak(known, tick, std::memory_order_release,
                                                           std::memory_order_acquire)) {
    }
}

void MasterSemaphore::Refresh() {
    auto [counter_result, counter] = instance.GetDevice().getSemaphoreCounterValue(*semaphore);
    ASSERT_MSG(counter_result == vk::Result::eSuccess, "Failed to get master semaphore value: {}",
               vk::to_string(counter_result));
    // The counter of an emulated timeline can come out lower than it once was (see Wait): what
    // has been reached stays reached.
    NoteReached(counter);
}

bool MasterSemaphore::Wait(u64 tick, std::chrono::nanoseconds limit) {
    // No need to wait if the GPU is ahead of the tick
    if (IsFree(tick)) {
        return true;
    }
    // Update the GPU tick and try again
    Refresh();
    if (IsFree(tick)) {
        return true;
    }

    if (WaitsDirectly()) {
        const vk::SemaphoreWaitInfo wait_info = {
            .semaphoreCount = 1,
            .pSemaphores = &semaphore.get(),
            .pValues = &tick,
        };
        while (instance.GetDevice().waitSemaphores(&wait_info, std::numeric_limits<u64>::max()) !=
               vk::Result::eSuccess) {
        }
        Refresh();
        return true;
    }

    // Drivers without timeline semaphores of their own (Turnip on the Adreno kernel driver) get
    // Mesa's emulation: a list of the submissions that are under way, each with a fence. A wait
    // for a tick waits for the fence of that tick's submission and then takes it off the list
    // as the newest one finished, whatever is still on the list in front of it. When those
    // older entries are cleared out in their turn, the timeline's value goes *back* to theirs,
    // and the tick that was waited for is forgotten: asking for its value returns less than it,
    // and waiting for it a second time waits for a submission that has yet to be made. Made by
    // the thread that is waiting, that is for ever (seen on the headset as a game stuck in its
    // loading screen, the first time the GPU was a few submissions behind).
    //
    // Two rules keep that from happening. Ticks are waited for one after the other, so that the
    // driver always finishes the oldest entry of its list and its value only ever rises. And
    // what a wait has established is remembered here: the driver is never asked again about a
    // tick it has once reported as reached.
    using Clock = std::chrono::steady_clock;
    const auto started = Clock::now();
    auto complained = started;
    while (!IsFree(tick)) {
        const u64 next = KnownGpuTick() + 1;
        const vk::SemaphoreWaitInfo wait_info = {
            .semaphoreCount = 1,
            .pSemaphores = &semaphore.get(),
            .pValues = &next,
        };
        const vk::Result result = instance.GetDevice().waitSemaphores(&wait_info, WaitSliceNs);
        if (result == vk::Result::eSuccess) {
            NoteReached(next);
            continue;
        }
        if (result != vk::Result::eTimeout) {
            LOG_CRITICAL(Render_Vulkan, "Waiting for tick {} failed: {}", next,
                         vk::to_string(result));
            return false;
        }
        // Another thread may have learned more in the meantime.
        Refresh();
        const auto now = Clock::now();
        if (now - started >= limit && !IsFree(tick)) {
            return false;
        }
        // A tick that has been submitted is a matter of milliseconds; one that has not (work
        // put off until a tick that is still being recorded) takes as long as it takes.
        if (next < CurrentTick() && now - complained >= std::chrono::seconds{5}) {
            complained = now;
            LOG_WARNING(Render_Vulkan,
                        "Waiting for tick {} since {:.1f} s: the GPU is known to have reached {}, "
                        "{} is being recorded",
                        tick, std::chrono::duration<double>(now - started).count(),
                        KnownGpuTick(), CurrentTick());
        }
    }
    return true;
}

void MasterSemaphore::Wait(u64 tick) {
    Wait(tick, std::chrono::nanoseconds::max());
}

} // namespace Vulkan
