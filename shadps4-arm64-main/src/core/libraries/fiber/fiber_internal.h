// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "common/arch.h"
#include "core/libraries/fiber/fiber.h"

namespace Core::GuestCpu {
struct HleCallFrame;
}

namespace Libraries::Fiber {

inline constexpr u32 kFiberSignature0 = 0xdef1649c;
inline constexpr u32 kFiberSignature1 = 0xb37592a0;
inline constexpr u64 kFiberStackSignature = 0x7149f2ca7149f2ca;

/// The context of the thread a fiber is running on, or null when the thread runs no fiber.
OrbisFiberContext* GetFiberContext();

s32 PS4_SYSV_ABI _sceFiberAttachContext(OrbisFiber* fiber, void* addr_context, u64 size_context);
void PS4_SYSV_ABI _sceFiberCheckStackOverflow(OrbisFiberContext* ctx);

#ifndef ARCH_X86_64
/// Context switching for guests that run under FEX. Each function is the body of the fiber
/// function of the same name and receives the guest's registers as they were at the call.
namespace Fex {

void Run(Core::GuestCpu::HleCallFrame& frame);
void AttachContextAndRun(Core::GuestCpu::HleCallFrame& frame);
void Switch(Core::GuestCpu::HleCallFrame& frame);
void AttachContextAndSwitch(Core::GuestCpu::HleCallFrame& frame);
void ReturnToThread(Core::GuestCpu::HleCallFrame& frame);

/// Forgets the suspended state of a fiber that is being finalized or initialized again.
void Discard(const OrbisFiber* fiber);

} // namespace Fex
#endif

} // namespace Libraries::Fiber
