// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <mutex>
#include <optional>
#include <unordered_map>

#include "common/assert.h"
#include "common/logging/log.h"
#include "common/singleton.h"
#include "core/guest_cpu/hle_call_adapter.h"
#include "core/libraries/fiber/fiber_error.h"
#include "core/libraries/fiber/fiber_internal.h"
#include "core/linker.h"
#include "core/tls.h"

// With FEX the emulator never runs on the guest's stack. An HLE call arrives as a frame of guest
// registers, and whatever the frame holds when the call returns is what the guest carries on
// with. Switching fibers therefore takes no assembly: save the callee-saved registers and the
// stack pointer of the context being left, load those of the context being entered, and return.
// The veneer the guest called through ends in `ret`, which pops the return address of whichever
// context was switched in.

namespace Libraries::Fiber::Fex {

namespace {

using Core::GuestCpu::HleCallFrame;

// Indices into HleCallFrame::gpr, which follows the x86 register encoding.
enum Gpr : std::size_t {
    Rax = 0,
    Rcx = 1,
    Rdx = 2,
    Rbx = 3,
    Rsp = 4,
    Rbp = 5,
    Rsi = 6,
    Rdi = 7,
    R8 = 8,
    R12 = 12,
    R13 = 13,
    R14 = 14,
    R15 = 15,
};

/// A fiber that gave up the thread from inside sceFiberSwitch or sceFiberReturnToThread.
struct SuspendedFiber {
    OrbisFiberContext registers{};
    /// Where that call reports the argument the fiber is resumed with.
    u64* arg_on_run{};
};

/// The thread side of sceFiberRun: what tcb_fiber points at while fibers run on the thread.
struct ThreadState {
    OrbisFiberContext context{};
    u64* arg_on_return{};
};

std::mutex g_suspended_mutex;
std::unordered_map<const OrbisFiber*, SuspendedFiber> g_suspended;
thread_local ThreadState g_thread_state;

void SaveRegisters(const HleCallFrame& frame, OrbisFiberContext& context) {
    context.rbx = frame.gpr[Rbx];
    context.rsp = frame.gpr[Rsp];
    context.rbp = frame.gpr[Rbp];
    context.r12 = frame.gpr[R12];
    context.r13 = frame.gpr[R13];
    context.r14 = frame.gpr[R14];
    context.r15 = frame.gpr[R15];
}

void LoadRegisters(HleCallFrame& frame, const OrbisFiberContext& context) {
    frame.gpr[Rbx] = context.rbx;
    frame.gpr[Rsp] = context.rsp;
    frame.gpr[Rbp] = context.rbp;
    frame.gpr[R12] = context.r12;
    frame.gpr[R13] = context.r13;
    frame.gpr[R14] = context.r14;
    frame.gpr[R15] = context.r15;
}

void SetResult(HleCallFrame& frame, s32 result) {
    frame.gpr[Rax] = static_cast<u64>(result);
}

void Suspend(const HleCallFrame& frame, OrbisFiber* fiber, u64* arg_on_run) {
    std::scoped_lock lock{g_suspended_mutex};
    SuspendedFiber& suspended = g_suspended[fiber];
    SaveRegisters(frame, suspended.registers);
    suspended.arg_on_run = arg_on_run;
    fiber->context = &suspended.registers;
}

std::optional<SuspendedFiber> Resume(OrbisFiber* fiber) {
    std::scoped_lock lock{g_suspended_mutex};
    const auto it = g_suspended.find(fiber);
    if (it == g_suspended.end()) {
        return std::nullopt;
    }
    const SuspendedFiber suspended = it->second;
    g_suspended.erase(it);
    fiber->context = nullptr;
    return suspended;
}

void EntryReturned(HleCallFrame&) {
    UNREACHABLE_MSG("Fiber entry function returned.");
}

VAddr EntryReturnAddress() {
    static const VAddr veneer = Common::Singleton<Core::Linker>::Instance()->AllocateHleVeneer(
        Core::GuestCpu::MakeRawHleCallAdapter(EntryReturned), "bachata.fiber_entry_returned");
    return veneer;
}

/// Gives the thread to `fiber`. It continues where it suspended itself, or starts its entry
/// function if it never ran; `previous` is the fiber the thread was running until now.
void Enter(HleCallFrame& frame, OrbisFiber* fiber, u64 arg_on_run_to, OrbisFiberContext* thread,
           OrbisFiber* previous) {
    thread->current_fiber = fiber;
    thread->prev_fiber = nullptr;
    thread->arg_on_run_to = arg_on_run_to;
    if (previous != nullptr) {
        // Its registers are saved and nothing here touches its stack any more, so another
        // thread may pick it up from now on.
        previous->state = FiberState::Idle;
    }

    if (const auto suspended = Resume(fiber)) {
        if (suspended->arg_on_run != nullptr) {
            *suspended->arg_on_run = arg_on_run_to;
        }
        LoadRegisters(frame, suspended->registers);
        SetResult(frame, ORBIS_OK);
        return;
    }

    // A fiber without a context of its own borrows the stack of the thread that runs it.
    const u64 stack_top = fiber->addr_context != nullptr
                              ? reinterpret_cast<u64>(fiber->addr_context) + fiber->size_context
                              : thread->rsp;
    u64* stack = reinterpret_cast<u64*>(stack_top & ~15ULL);
    // The veneer's `ret` pops the entry point, which then finds the stack as a call left it.
    *--stack = EntryReturnAddress();
    *--stack = reinterpret_cast<u64>(fiber->entry);
    frame.gpr[Rsp] = reinterpret_cast<u64>(stack);
    frame.gpr[Rbp] = 0;
    frame.gpr[Rdi] = fiber->arg_on_initialize;
    frame.gpr[Rsi] = arg_on_run_to;
    SetResult(frame, ORBIS_OK);
}

s32 CheckFiber(const OrbisFiber* fiber, const void* addr_context) {
    if (!fiber) {
        return ORBIS_FIBER_ERROR_NULL;
    }
    if (reinterpret_cast<u64>(fiber) & 7 || reinterpret_cast<u64>(addr_context) & 15) {
        return ORBIS_FIBER_ERROR_ALIGNMENT;
    }
    if (fiber->magic_start != kFiberSignature0 || fiber->magic_end != kFiberSignature1) {
        return ORBIS_FIBER_ERROR_INVALID;
    }
    return ORBIS_OK;
}

void RunImpl(HleCallFrame& frame, OrbisFiber* fiber, void* addr_context, u64 size_context,
             u64 arg_on_run_to, u64* arg_on_return) {
    if (const s32 result = CheckFiber(fiber, addr_context); result != ORBIS_OK) {
        return SetResult(frame, result);
    }

    Core::Tcb* tcb = Core::GetTcbBase();
    if (tcb->tcb_fiber) {
        return SetResult(frame, ORBIS_FIBER_ERROR_PERMISSION);
    }

    /* Caller wants to attach context and run. */
    if (addr_context != nullptr || size_context != 0) {
        const s32 result = _sceFiberAttachContext(fiber, addr_context, size_context);
        if (result < 0) {
            return SetResult(frame, result);
        }
    }

    FiberState expected = FiberState::Idle;
    if (!fiber->state.compare_exchange_strong(expected, FiberState::Run)) {
        return SetResult(frame, ORBIS_FIBER_ERROR_STATE);
    }

    ThreadState& thread = g_thread_state;
    thread.context = {};
    thread.arg_on_return = arg_on_return;
    SaveRegisters(frame, thread.context);
    tcb->tcb_fiber = &thread.context;

    Enter(frame, fiber, arg_on_run_to, &thread.context, nullptr);
}

void SwitchImpl(HleCallFrame& frame, OrbisFiber* fiber, void* addr_context, u64 size_context,
                u64 arg_on_run_to, u64* arg_on_run) {
    if (const s32 result = CheckFiber(fiber, addr_context); result != ORBIS_OK) {
        return SetResult(frame, result);
    }

    OrbisFiberContext* thread = GetFiberContext();
    if (!thread) {
        return SetResult(frame, ORBIS_FIBER_ERROR_PERMISSION);
    }

    /* Caller wants to attach context and switch. */
    if (addr_context != nullptr || size_context != 0) {
        const s32 result = _sceFiberAttachContext(fiber, addr_context, size_context);
        if (result < 0) {
            return SetResult(frame, result);
        }
    }

    FiberState expected = FiberState::Idle;
    if (!fiber->state.compare_exchange_strong(expected, FiberState::Run)) {
        return SetResult(frame, ORBIS_FIBER_ERROR_STATE);
    }

    // A fiber on a borrowed stack cannot be resumed, there is nothing to keep of it.
    OrbisFiber* current = thread->current_fiber;
    if (current->addr_context != nullptr) {
        _sceFiberCheckStackOverflow(thread);
        Suspend(frame, current, arg_on_run);
    }

    Enter(frame, fiber, arg_on_run_to, thread, current);
}

} // namespace

void Run(HleCallFrame& frame) {
    RunImpl(frame, reinterpret_cast<OrbisFiber*>(frame.gpr[Rdi]), nullptr, 0, frame.gpr[Rsi],
            reinterpret_cast<u64*>(frame.gpr[Rdx]));
}

void AttachContextAndRun(HleCallFrame& frame) {
    RunImpl(frame, reinterpret_cast<OrbisFiber*>(frame.gpr[Rdi]),
            reinterpret_cast<void*>(frame.gpr[Rsi]), frame.gpr[Rdx], frame.gpr[Rcx],
            reinterpret_cast<u64*>(frame.gpr[R8]));
}

void Switch(HleCallFrame& frame) {
    SwitchImpl(frame, reinterpret_cast<OrbisFiber*>(frame.gpr[Rdi]), nullptr, 0, frame.gpr[Rsi],
               reinterpret_cast<u64*>(frame.gpr[Rdx]));
}

void AttachContextAndSwitch(HleCallFrame& frame) {
    SwitchImpl(frame, reinterpret_cast<OrbisFiber*>(frame.gpr[Rdi]),
               reinterpret_cast<void*>(frame.gpr[Rsi]), frame.gpr[Rdx], frame.gpr[Rcx],
               reinterpret_cast<u64*>(frame.gpr[R8]));
}

void ReturnToThread(HleCallFrame& frame) {
    const u64 arg_on_return = frame.gpr[Rdi];
    u64* arg_on_run = reinterpret_cast<u64*>(frame.gpr[Rsi]);

    OrbisFiberContext* thread = GetFiberContext();
    if (!thread) {
        return SetResult(frame, ORBIS_FIBER_ERROR_PERMISSION);
    }

    OrbisFiber* current = thread->current_fiber;
    if (current->addr_context != nullptr) {
        _sceFiberCheckStackOverflow(thread);
        Suspend(frame, current, arg_on_run);
    }

    // The thread continues after its sceFiberRun call.
    ThreadState& state = g_thread_state;
    ASSERT_MSG(thread == &state.context, "Fiber returned to a thread that is not running it");
    thread->current_fiber = nullptr;
    current->state = FiberState::Idle;
    if (state.arg_on_return != nullptr) {
        *state.arg_on_return = arg_on_return;
    }
    Core::GetTcbBase()->tcb_fiber = nullptr;

    LoadRegisters(frame, state.context);
    SetResult(frame, ORBIS_OK);
}

void Discard(const OrbisFiber* fiber) {
    std::scoped_lock lock{g_suspended_mutex};
    g_suspended.erase(fiber);
}

} // namespace Libraries::Fiber::Fex
