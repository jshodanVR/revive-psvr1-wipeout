// SPDX-License-Identifier: MIT
#pragma once

#include <chrono>
#include <string>

#include "common/types.h"

namespace Core::GuestCpu {

/// Tells the watchdog that the guest is alive: it has just handed over a frame. The first call
/// starts the watchdog, which reports what every guest thread is doing when frames stop coming
/// for a while, or whenever a file named "dump_threads" appears in the user folder.
void NoteGuestProgress() noexcept;

/// How many frames the guest has handed over so far.
u64 GuestFrameCount() noexcept;

/// Waits until the guest has handed over more than `count` frames, at most for `timeout`.
/// Returns the number of frames handed over by then.
u64 WaitForGuestFrame(u64 count, std::chrono::milliseconds timeout);

/// One line per guest thread: the HLE call it is in and who made that call, or that it is
/// running guest code. Empty when the guest does not run on the FEX CPU.
std::string DescribeGuestThreads();

} // namespace Core::GuestCpu
