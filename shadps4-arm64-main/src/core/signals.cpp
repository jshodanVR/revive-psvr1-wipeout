// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <cstring>
#include <cstdio>
#include <cstdlib>
#include "common/arch.h"
#include "common/assert.h"
#include "common/crash_reporter.h"
#include "common/decoder.h"
#include "common/elf_info.h"
#include "common/memory_patcher.h"
#include "common/signal_context.h"
#include "core/libraries/kernel/threads/exception.h"
#include "core/signals.h"
#ifdef SHADPS4_ENABLE_FEX_GUEST_CPU
#include "core/fex/fex_guest_engine.h"
#endif
#include "emulator.h"

#ifdef _WIN32
#include <windows.h>
static constexpr DWORD MS_VC_EXCEPTION = 0x406D1388;
#else
#include <csignal>
#include <pthread.h>
#include <unistd.h>
#ifdef ARCH_X86_64
#include <Zydis/Formatter.h>
#endif
#endif

#ifndef _WIN32
namespace Libraries::Kernel {
void SigactionHandler(int native_signum, siginfo_t* inf, ucontext_t* raw_context);
extern std::array<OrbisKernelExceptionHandler, 32> Handlers;
} // namespace Libraries::Kernel
#endif

namespace Core {

#if defined(_WIN32)

static LONG WINAPI SignalHandler(EXCEPTION_POINTERS* pExp) noexcept {
    const auto* signals = Signals::Instance();
    DWORD code = 0;
    PVOID address = nullptr;

    if (pExp != nullptr && pExp->ExceptionRecord != nullptr) {
        code = pExp->ExceptionRecord->ExceptionCode;
        address = pExp->ExceptionRecord->ExceptionAddress;
    }

    bool handled = false;
    switch (code) {
    case EXCEPTION_ACCESS_VIOLATION:
        handled = signals->DispatchAccessViolation(
            pExp, reinterpret_cast<void*>(pExp->ExceptionRecord->ExceptionInformation[1]));
        break;
    case EXCEPTION_ILLEGAL_INSTRUCTION:
        handled = signals->DispatchIllegalInstruction(pExp);
        break;
    case DBG_PRINTEXCEPTION_C:
    case DBG_PRINTEXCEPTION_WIDE_C:
        // Used by OutputDebugString functions.
        return EXCEPTION_CONTINUE_EXECUTION;
    case 0xe06d7363: // MSVC C++ exception, observed before any enclosing catch runs.
        // This is a vectored (first-chance) handler. A filesystem error handled by
        // the save backup worker must not shut down emulation before its catch.
        return EXCEPTION_CONTINUE_SEARCH;
    case MS_VC_EXCEPTION:
        LOG_DEBUG(Debug, "Pass MS_VC_EXCEPTION at {} to handler", address);
        return EXCEPTION_EXECUTE_HANDLER;
    default:
        break;
    }

    if (handled) {
        return EXCEPTION_CONTINUE_EXECUTION;
    }

    // Breakpoints almost certainly come from our asserts/unreachables, no need to log it again.
    if (code != EXCEPTION_BREAKPOINT) {
#ifdef ARCH_X86_64
        if (std::getenv("BACHATA_CRASH_REGISTERS") && pExp && pExp->ContextRecord) {
            const auto& c = *pExp->ContextRecord;
            // Write synchronously: an unhandled guest fault can exit before the log drains.
            std::fprintf(stderr,
                         "[GuestFault] code=%lx rip=%llx access=%llu address=%llx "
                         "rax=%llx rbx=%llx rcx=%llx rdx=%llx rsi=%llx rdi=%llx "
                         "rbp=%llx rsp=%llx r8=%llx r9=%llx r10=%llx r11=%llx "
                         "r12=%llx r13=%llx r14=%llx r15=%llx\n",
                         code, c.Rip,
                         pExp->ExceptionRecord->NumberParameters > 0 ?
                             pExp->ExceptionRecord->ExceptionInformation[0] : 0,
                         pExp->ExceptionRecord->NumberParameters > 1 ?
                             pExp->ExceptionRecord->ExceptionInformation[1] : 0,
                         c.Rax, c.Rbx, c.Rcx, c.Rdx, c.Rsi, c.Rdi, c.Rbp, c.Rsp,
                         c.R8, c.R9, c.R10, c.R11, c.R12, c.R13, c.R14, c.R15);
            std::fflush(stderr);
        }
#endif
        LOG_CRITICAL(Debug, "Unhandled Exception code {:#x} at {}", code, address);
        if (code == EXCEPTION_ACCESS_VIOLATION && pExp && pExp->ExceptionRecord &&
            pExp->ExceptionRecord->NumberParameters >= 2) {
            LOG_CRITICAL(Debug, "Access violation operation={} target={:#x}",
                         pExp->ExceptionRecord->ExceptionInformation[0],
                         pExp->ExceptionRecord->ExceptionInformation[1]);
        }
#if defined(ARCH_X86_64)
        if (pExp && pExp->ContextRecord) {
            const auto& c = *pExp->ContextRecord;
            LOG_CRITICAL(Debug,
                         "Fault context rip={:#x} rsp={:#x} rbp={:#x} rax={:#x} rbx={:#x} "
                         "rcx={:#x} rdx={:#x} rsi={:#x} rdi={:#x} r8={:#x} r9={:#x}",
                         c.Rip, c.Rsp, c.Rbp, c.Rax, c.Rbx, c.Rcx, c.Rdx, c.Rsi, c.Rdi,
                         c.R8, c.R9);
            LOG_CRITICAL(Debug,
                         "Fault context r10={:#x} r11={:#x} r12={:#x} r13={:#x} r14={:#x} r15={:#x}",
                         c.R10, c.R11, c.R12, c.R13, c.R14, c.R15);
            if (Common::ElfInfo::Instance().GameSerial() == "CUSA02350" &&
                c.Rip == MemoryPatcher::g_eboot_address + 0x1fa81) {
                // Rush 01.00: deferred resource destruction passed an invalid
                // pointer to its allocator. Preserve the allocation evidence;
                // do not skip the free or claim that more memory fixes it.
                const auto snapshot = [](const char* label, u64 ptr, SIZE_T words) {
                    u64 values[64]{};
                    SIZE_T copied{};
                    if (!ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(ptr),
                                           values, words * sizeof(u64), &copied)) {
                        LOG_CRITICAL(Debug, "Rush allocation fault: {} at {:#x} unreadable", label, ptr);
                        return;
                    }
                    for (SIZE_T i = 0; i < copied / sizeof(u64); ++i) {
                        LOG_CRITICAL(Debug, "Rush allocation fault: {} +{:#x} = {:#x}",
                                     label, i * sizeof(u64), values[i]);
                    }
                };
                snapshot("heap", c.Rbx, 7);
                snapshot("deferred resource", c.R12, 11);
                snapshot("pool configuration", MemoryPatcher::g_eboot_address + 0x24437c0, 64);
                snapshot("render list roots", MemoryPatcher::g_eboot_address + 0x2440968, 2);
            }
            // SysV leaf functions keep live locals below RSP. These include the
            // saved output pointer in Farpoint's failing index-generation routine.
            // Only read on a fatal fault; never dereference a possibly damaged stack.
            if (c.Rsp >= 128) {
                u64 red_zone[16]{};
                SIZE_T red_zone_read{};
                if (ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(c.Rsp - 128),
                                      red_zone, sizeof(red_zone), &red_zone_read)) {
                    for (SIZE_T i = 0; i < red_zone_read / sizeof(u64); ++i) {
                        LOG_CRITICAL(Debug, "Fault stack -{:#x}: {:#x}",
                                     128 - i * sizeof(u64), red_zone[i]);
                    }
                }
            }
            // Read safely: an invalid guest stack must not fault the handler again.
            // Include return addresses above larger guest frames (the WipEout
            // model crash's caller was at +0x88, outside the old 0x80 dump).
            u64 stack[64]{};
            SIZE_T bytes_read{};
            if (ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(c.Rsp),
                                  stack, sizeof(stack), &bytes_read)) {
                for (SIZE_T i = 0; i < bytes_read / sizeof(u64); ++i) {
                    LOG_CRITICAL(Debug, "Fault stack +{:#x}: {:#x}", i * sizeof(u64), stack[i]);
                }
            }
            MEMORY_BASIC_INFORMATION region{};
            if (VirtualQuery(address, &region, sizeof(region))) {
                LOG_CRITICAL(Debug, "Fault code region base={} allocation={} size={:#x} "
                                    "protection={:#x} type={:#x}",
                             region.BaseAddress, region.AllocationBase, region.RegionSize,
                             region.Protect, region.Type);
            }
            // CaptureStackBackTrace below describes this exception handler, not
            // the faulting guest. Recover bounded frame-pointer candidates from
            // the saved context as additional evidence; optimized frames may stop
            // the chain. Never dereference guest pointers directly in a handler.
            u64 frame = c.Rbp;
            for (u32 depth = 0; depth < 24 && frame >= c.Rsp &&
                                frame - c.Rsp < 1024 * 1024 && (frame & 7) == 0; ++depth) {
                u64 record[2]{};
                SIZE_T read{};
                if (!ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(frame),
                                       record, sizeof(record), &read) || read != sizeof(record)) {
                    break;
                }
                LOG_CRITICAL(Debug, "Fault frame candidate {}: frame={:#x} return={:#x}",
                             depth, frame, record[1]);
                if (record[0] <= frame) {
                    break;
                }
                frame = record[0];
            }
        }
#endif
        // Where it came from: each caller as its module and the place in it.
        void* frames[32];
        const USHORT count = CaptureStackBackTrace(0, 32, frames, nullptr);
        for (USHORT i = 0; i < count; ++i) {
            HMODULE module = nullptr;
            char name[MAX_PATH] = "?";
            if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                       GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                   static_cast<LPCSTR>(frames[i]), &module)) {
                GetModuleFileNameA(module, name, sizeof(name));
            }
            const char* file = std::strrchr(name, '\\');
            LOG_CRITICAL(Debug, "  {} + {:#x}", file ? file + 1 : name,
                         reinterpret_cast<uintptr_t>(frames[i]) -
                             reinterpret_cast<uintptr_t>(module));
        }
        Common::Singleton<Core::Emulator>::Instance()->Shutdown();
    }

    return EXCEPTION_CONTINUE_SEARCH;
}

#else

static std::string DisassembleInstruction(void* code_address) {
    char buffer[256] = "<unable to decode>";

#ifdef ARCH_X86_64
    ZydisDecodedInstruction instruction;
    ZydisDecodedOperand operands[ZYDIS_MAX_OPERAND_COUNT];
    const auto status =
        Common::Decoder::Instance()->decodeInstruction(instruction, operands, code_address);
    if (ZYAN_SUCCESS(status)) {
        ZydisFormatter formatter;
        ZydisFormatterInit(&formatter, ZYDIS_FORMATTER_STYLE_INTEL);
        ZydisFormatterFormatInstruction(&formatter, &instruction, operands,
                                        instruction.operand_count_visible, buffer, sizeof(buffer),
                                        reinterpret_cast<u64>(code_address), ZYAN_NULL);
    }
#endif

    return buffer;
}

#ifdef SHADPS4_ENABLE_FEX_GUEST_CPU
/// Reads a word of guest memory that may not be mapped. The kernel does the copy, so a bad
/// address is an error return rather than a second fault inside the fault handler.
static bool ReadGuestWord(u64 address, u64* value) {
    static int pipe_ends[2] = {-1, -1};
    if (pipe_ends[0] < 0 && pipe(pipe_ends) != 0) {
        return false;
    }
    if (write(pipe_ends[1], reinterpret_cast<const void*>(address), sizeof(*value)) !=
        static_cast<ssize_t>(sizeof(*value))) {
        return false;
    }
    return read(pipe_ends[0], value, sizeof(*value)) == static_cast<ssize_t>(sizeof(*value));
}

/// The return addresses up the guest's call stack, found through its frame pointers.
static std::string GuestCallers(u64 frame) {
    std::string callers;
    for (int depth = 0; depth < 24 && frame != 0 && (frame & 7) == 0; ++depth) {
        u64 next = 0;
        u64 return_address = 0;
        if (!ReadGuestWord(frame, &next) || !ReadGuestWord(frame + 8, &return_address)) {
            break;
        }
        callers += fmt::format(" {:#x}", return_address);
        if (next <= frame) {
            break;
        }
        frame = next;
    }
    return callers;
}
#endif

void SignalHandler(int sig, siginfo_t* info, void* raw_context) {
    Common::ReportCrash(raw_context, sig, info);
    const auto* signals = Signals::Instance();

    auto* code_address = Common::GetRip(raw_context);

    switch (sig) {
    case SIGBUS:
    case SIGSEGV: {
#ifdef SHADPS4_ENABLE_FEX_GUEST_CPU
        if (sig == SIGBUS && ::Core::Fex::HandleGuestSignal(sig, info, raw_context)) {
            return;
        }
#endif
        const bool is_write = Common::IsWriteError(raw_context);
        if (!signals->DispatchAccessViolation(raw_context, info->si_addr)) {
            // If the guest has installed a custom signal handler, and the access violation didn't
            // come from HLE memory tracking, pass the signal on
            if (Libraries::Kernel::Handlers[Libraries::Kernel::NativeToOrbisSignal(sig)]) {
                Libraries::Kernel::SigactionHandler(sig, info,
                                                    reinterpret_cast<ucontext_t*>(raw_context));
                return;
            }
#ifdef SHADPS4_ENABLE_FEX_GUEST_CPU
            uint64_t guest_rip = 0;
            uint64_t guest_rax = 0;
            if (::Core::Fex::BachataQueryGuestRipSyscall(&guest_rip, &guest_rax)) {
                LOG_CRITICAL(Debug, "FEX guest state at fault: rip={:#x} rax={:#x}", guest_rip,
                             guest_rax);
            }
            // When the fault is inside an HLE function this tells which call it was: the first
            // arguments, and who made it.
            uint64_t gprs[16]{};
            if (::Core::Fex::BachataQueryGuestRegisters(gprs)) {
                const uint64_t rsp = gprs[4];
                uint64_t return_address = 0;
                if ((rsp & 7) == 0) {
                    ReadGuestWord(rsp, &return_address);
                }
                LOG_CRITICAL(Debug,
                             "FEX guest registers: rdi={:#x} rsi={:#x} rdx={:#x} rcx={:#x} "
                             "rsp={:#x} return address={:#x}",
                             gprs[7], gprs[6], gprs[2], gprs[1], rsp, return_address);
                LOG_CRITICAL(Debug, "FEX guest callers:{}", GuestCallers(gprs[5]));
            }
#endif
            UNREACHABLE_MSG("Unhandled access violation at code address {}: {} address {}",
                            fmt::ptr(code_address), is_write ? "Write to" : "Read from",
                            fmt::ptr(info->si_addr));
        }
        break;
    }
    case SIGILL:
        if (!signals->DispatchIllegalInstruction(raw_context)) {
            if (Libraries::Kernel::Handlers[Libraries::Kernel::NativeToOrbisSignal(sig)]) {
                Libraries::Kernel::SigactionHandler(sig, info,
                                                    reinterpret_cast<ucontext_t*>(raw_context));
                return;
            }
            UNREACHABLE_MSG("Unhandled illegal instruction at code address {}: {}",
                            fmt::ptr(code_address), DisassembleInstruction(code_address));
        }
        break;
    default:
        if (sig == SIGSLEEP) {
            // Sleep thread until signal is received again
            sigset_t sigset;
            sigemptyset(&sigset);
            sigaddset(&sigset, SIGSLEEP);
            sigwait(&sigset, &sig);
        }
        break;
    }
}

#endif

SignalDispatch::SignalDispatch() {
    Common::InitCrashReporter();
#if defined(_WIN32)
    ASSERT_MSG(handle = AddVectoredExceptionHandler(0, SignalHandler),
               "Failed to register exception handler.");
#else
    struct sigaction action{};
    action.sa_sigaction = SignalHandler;
    action.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&action.sa_mask);

    ASSERT_MSG(sigaction(SIGSEGV, &action, nullptr) == 0 &&
                   sigaction(SIGBUS, &action, nullptr) == 0,
               "Failed to register access violation signal handler.");
    ASSERT_MSG(sigaction(SIGILL, &action, nullptr) == 0,
               "Failed to register illegal instruction signal handler.");
    ASSERT_MSG(sigaction(SIGSLEEP, &action, nullptr) == 0,
               "Failed to register sleep signal handler.");
#endif
}

SignalDispatch::~SignalDispatch() {
#if defined(_WIN32)
    ASSERT_MSG(RemoveVectoredExceptionHandler(handle), "Failed to remove exception handler.");
#else
    struct sigaction action{};
    action.sa_handler = SIG_DFL;
    action.sa_flags = 0;
    sigemptyset(&action.sa_mask);

    ASSERT_MSG(sigaction(SIGSEGV, &action, nullptr) == 0 &&
                   sigaction(SIGBUS, &action, nullptr) == 0,
               "Failed to remove access violation signal handler.");
    ASSERT_MSG(sigaction(SIGILL, &action, nullptr) == 0,
               "Failed to remove illegal instruction signal handler.");
#endif
}

bool SignalDispatch::DispatchAccessViolation(void* context, void* fault_address) const {
    for (const auto& [handler, _] : access_violation_handlers) {
        if (handler(context, fault_address)) {
            return true;
        }
    }
    return false;
}

bool SignalDispatch::DispatchIllegalInstruction(void* context) const {
    for (const auto& [handler, _] : illegal_instruction_handlers) {
        if (handler(context)) {
            return true;
        }
    }
    return false;
}

} // namespace Core
