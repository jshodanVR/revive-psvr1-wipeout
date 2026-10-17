// SPDX-License-Identifier: MIT

#include "guest_watchdog.h"

#include <atomic>
#include <cstdlib>
#include <vector>
#include <string>
#include <algorithm>
#include <condition_variable>
#include <filesystem>
#include <mutex>
#include <thread>

#include "common/logging/log.h"
#include "common/path_util.h"
#include "common/thread.h"

#if defined(__linux__) && defined(__aarch64__)
#define WATCHDOG_NATIVE_STACKS 1
#include <csignal>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <dlfcn.h>
#include <link.h>
#include <semaphore.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <ucontext.h>
#include <unistd.h>
#endif

namespace Core::GuestCpu {

namespace {

using Clock = std::chrono::steady_clock;

constexpr auto StallTime = std::chrono::seconds{20};

std::atomic<Clock::rep> last_progress{};
std::once_flag start_flag;

std::mutex frame_mutex;
std::condition_variable frame_signal;
u64 frame_count{};

#ifdef WATCHDOG_NATIVE_STACKS
// Where every thread of the emulator is, guest or not: each one is interrupted with a signal and
// walks its own frame pointers. Addresses inside the emulator are printed relative to where it
// is loaded, ready for a symbolizer: llvm-symbolizer -e shadps4 0x...
constexpr int MaxFrames = 20;

struct StackSample {
    uintptr_t frames[MaxFrames];
    int count;
};

StackSample sample;
sem_t sample_done;
int sample_pipe[2] = {-1, -1};
int sample_signal = 0;
uintptr_t image_begin = 0;
uintptr_t image_end = 0;

/// Reads two words that may not be mapped. The kernel does the copy, so a bad frame pointer,
/// which is what generated code leaves behind, is an error and not a fault.
bool ReadFrame(uintptr_t address, uintptr_t out[2]) {
    if (write(sample_pipe[1], reinterpret_cast<const void*>(address), 2 * sizeof(uintptr_t)) !=
        static_cast<ssize_t>(2 * sizeof(uintptr_t))) {
        return false;
    }
    return read(sample_pipe[0], out, 2 * sizeof(uintptr_t)) ==
           static_cast<ssize_t>(2 * sizeof(uintptr_t));
}

void SampleHandler(int, siginfo_t*, void* raw_context) {
    const int saved_errno = errno;
    const auto* context = static_cast<const ucontext_t*>(raw_context);
    uintptr_t frame = context->uc_mcontext.regs[29];
    const uintptr_t stack = context->uc_mcontext.sp;
    sample.count = 0;
    sample.frames[sample.count++] = context->uc_mcontext.pc;
    sample.frames[sample.count++] = context->uc_mcontext.regs[30];
    while (sample.count < MaxFrames && (frame & 15) == 0 && frame >= stack &&
           frame - stack < (64u << 20)) {
        uintptr_t record[2];
        if (!ReadFrame(frame, record)) {
            break;
        }
        sample.frames[sample.count++] = record[1];
        if (record[0] <= frame) {
            break;
        }
        frame = record[0];
    }
    sem_post(&sample_done);
    errno = saved_errno;
}

bool SetupSampling() {
    static bool ready = false;
    static bool attempted = false;
    if (attempted) {
        return ready;
    }
    attempted = true;

    dl_iterate_phdr(
        [](dl_phdr_info* info, size_t, void*) {
            // The first entry is the program itself.
            for (int i = 0; i < info->dlpi_phnum; ++i) {
                const auto& header = info->dlpi_phdr[i];
                if (header.p_type != PT_LOAD) {
                    continue;
                }
                const uintptr_t begin = info->dlpi_addr + header.p_vaddr;
                image_begin = image_begin == 0 ? info->dlpi_addr : image_begin;
                image_end = std::max(image_end, begin + header.p_memsz);
            }
            return 1;
        },
        nullptr);

    sample_signal = SIGRTMIN + 9;
    struct sigaction action {};
    action.sa_sigaction = SampleHandler;
    action.sa_flags = SA_SIGINFO | SA_RESTART;
    sigemptyset(&action.sa_mask);
    ready = pipe(sample_pipe) == 0 && sem_init(&sample_done, 0, 0) == 0 &&
            sigaction(sample_signal, &action, nullptr) == 0;
    return ready;
}

std::string DescribeNativeThreads(const char* only = "") {
    if (!SetupSampling()) {
        return {};
    }
    std::string text;
    const int pid = getpid();
    const int self = static_cast<int>(syscall(SYS_gettid));
    DIR* tasks = opendir("/proc/self/task");
    if (tasks == nullptr) {
        return {};
    }
    while (const dirent* entry = readdir(tasks)) {
        const int tid = std::atoi(entry->d_name);
        if (tid <= 0 || tid == self) {
            continue;
        }
        char path[64];
        char name[32] = "?";
        std::snprintf(path, sizeof(path), "/proc/self/task/%d/comm", tid);
        if (std::FILE* file = std::fopen(path, "r")) {
            if (std::fgets(name, sizeof(name), file) != nullptr) {
                name[std::strcspn(name, "\n")] = '\0';
            }
            std::fclose(file);
        }
        if (std::strncmp(name, only, std::strlen(only)) != 0) {
            continue;
        }

        // A thread that answered after its time was up must not be taken for this one.
        while (sem_trywait(&sample_done) == 0) {
        }
        if (syscall(SYS_tgkill, pid, tid, sample_signal) != 0) {
            continue;
        }
        timespec deadline{};
        clock_gettime(CLOCK_REALTIME, &deadline);
        deadline.tv_nsec += 200'000'000;
        if (deadline.tv_nsec >= 1'000'000'000) {
            deadline.tv_nsec -= 1'000'000'000;
            ++deadline.tv_sec;
        }
        if (sem_timedwait(&sample_done, &deadline) != 0) {
            text += fmt::format("  {:<16} tid {}: did not answer\n", name, tid);
            continue;
        }
        text += fmt::format("  {:<16} tid {}:", name, tid);
        for (int i = 0; i < sample.count; ++i) {
            const uintptr_t address = sample.frames[i];
            if (address >= image_begin && address < image_end) {
                text += fmt::format(" {:#x}", address - image_begin);
            } else if (Dl_info info{}; dladdr(reinterpret_cast<void*>(address), &info) != 0 &&
                                         info.dli_fname != nullptr) {
                // A library: its name and the offset into it, and the function if it is exported.
                const char* file = std::strrchr(info.dli_fname, 0x2f);
                text += fmt::format(" [{}+{:#x}{}{}]", file != nullptr ? file + 1 : info.dli_fname,
                                    address - reinterpret_cast<uintptr_t>(info.dli_fbase),
                                    info.dli_sname != nullptr ? " " : "",
                                    info.dli_sname != nullptr ? info.dli_sname : "");
            } else {
                text += fmt::format(" [{:#x}]", address);
            }
        }
        text += '\n';
    }
    closedir(tasks);
    return text;
}
#else
std::string DescribeNativeThreads(const char* = "") {
    return {};
}
#endif

#ifdef WATCHDOG_NATIVE_STACKS
/// SHADPS4_THREAD_NICE="<name>=<nice>,<name>=<nice>..." gives the threads whose names begin with
/// <name> that scheduling priority (lower is more urgent). On a device with fewer processor
/// cores than the emulator has busy threads, that decides which of them wait: the thread the
/// sound comes from and the two a frame passes through should not be the ones.
void ApplyThreadPriorities() {
    struct Rule {
        std::string prefix;
        int nice;
    };
    static const std::vector<Rule> rules = [] {
        std::vector<Rule> parsed;
        const char* setting = std::getenv("SHADPS4_THREAD_NICE");
        std::string text = setting != nullptr ? setting : "";
        size_t begin = 0;
        while (begin < text.size()) {
            const size_t end = std::min(text.find(',', begin), text.size());
            const std::string entry = text.substr(begin, end - begin);
            const size_t equals = entry.rfind('=');
            if (equals != std::string::npos && equals > 0) {
                parsed.push_back({entry.substr(0, equals), std::atoi(entry.c_str() + equals + 1)});
            }
            begin = end + 1;
        }
        return parsed;
    }();
    static std::vector<int> seen;
    static std::vector<std::pair<int, std::string>> named;
    if (rules.empty()) {
        return;
    }
    DIR* tasks = opendir("/proc/self/task");
    if (tasks == nullptr) {
        return;
    }
    while (const dirent* entry = readdir(tasks)) {
        const int tid = std::atoi(entry->d_name);
        if (tid <= 0 || std::find(seen.begin(), seen.end(), tid) != seen.end()) {
            continue;
        }
        char path[64];
        char name[32] = "";
        std::snprintf(path, sizeof(path), "/proc/self/task/%d/comm", tid);
        if (std::FILE* file = std::fopen(path, "r")) {
            if (std::fgets(name, sizeof(name), file) != nullptr) {
                name[std::strcspn(name, "\n")] = 0;
            }
            std::fclose(file);
        }
        // A thread starts out with the name of the one that created it and is given its own a
        // moment later: only a name that is still the same on the next round counts.
        const auto earlier = std::find_if(named.begin(), named.end(),
                                          [tid](const auto& entry) { return entry.first == tid; });
        if (earlier == named.end()) {
            named.emplace_back(tid, name);
            continue;
        }
        if (earlier->second != name) {
            earlier->second = name;
            continue;
        }
        named.erase(earlier);
        seen.push_back(tid);
        for (const Rule& rule : rules) {
            if (std::strncmp(name, rule.prefix.c_str(), rule.prefix.size()) != 0) {
                continue;
            }
            const int result = setpriority(PRIO_PROCESS, static_cast<id_t>(tid), rule.nice);
            LOG_INFO(Core, "thread {} ({}): priority {}{}", name, tid, rule.nice,
                     result == 0 ? "" : " refused");
            break;
        }
    }
    closedir(tasks);
}
#else
void ApplyThreadPriorities() {}
#endif

void Watch() {
    Common::SetCurrentThreadName("shadPS4:Watchdog");
    const auto request =
        Common::FS::GetUserPath(Common::FS::PathType::UserDir) / "dump_threads";
    bool stall_reported = false;
    u32 round = 0;
    while (true) {
        std::this_thread::sleep_for(std::chrono::milliseconds{250});
        if (round++ % 8 == 0) {
            ApplyThreadPriorities();
        }

        const auto idle = Clock::now().time_since_epoch() - Clock::duration{last_progress.load()};
        const bool stalled = idle > StallTime;
        std::error_code ec;
        const bool requested = std::filesystem::exists(request, ec);
        if (!requested && (!stalled || stall_reported)) {
            stall_reported = stall_reported && stalled;
            continue;
        }
        stall_reported = stalled;

        const auto threads = DescribeGuestThreads();
        LOG_WARNING(Core, "{} s since the guest's last frame. Guest threads:\n{}",
                    std::chrono::duration_cast<std::chrono::seconds>(idle).count(),
                    threads.empty() ? "(not tracked on this CPU backend)" : threads);
        // A request file that holds a number asks for that many samples of every thread's
        // native stack, which is what tells where the emulator itself waits or works. A name
        // after the number limits them to the threads whose names begin with it: sampling one
        // thread is quick enough to do a few hundred times, which makes a profile of it.
        int samples = stalled ? 1 : 0;
        char only[32] = "";
        if (requested) {
            if (std::FILE* file = std::fopen(request.string().c_str(), "r")) {
                if (std::fscanf(file, "%d %31s", &samples, only) < 1) {
                    samples = 1;
                }
                std::fclose(file);
            }
            std::filesystem::remove(request, ec);
        }
        for (int i = 0; i < samples; ++i) {
            const auto native = DescribeNativeThreads(only);
            if (native.empty()) {
                break;
            }
            LOG_WARNING(Core, "Native stacks, sample {}:\n{}", i, native);
            // Not a multiple of anything a frame is made of.
            std::this_thread::sleep_for(
                std::chrono::microseconds{only[0] != 0 ? 5300 + (i % 7) * 410 : 37000});
        }
    }
}

} // namespace

void NoteGuestProgress() noexcept {
    last_progress.store(Clock::now().time_since_epoch().count(), std::memory_order_relaxed);
    {
        std::scoped_lock lock{frame_mutex};
        ++frame_count;
    }
    frame_signal.notify_all();
    std::call_once(start_flag, [] { std::thread{Watch}.detach(); });
}

u64 GuestFrameCount() noexcept {
    std::scoped_lock lock{frame_mutex};
    return frame_count;
}

u64 WaitForGuestFrame(u64 count, std::chrono::milliseconds timeout) {
    std::unique_lock lock{frame_mutex};
    frame_signal.wait_for(lock, timeout, [count] { return frame_count > count; });
    return frame_count;
}

#ifndef SHADPS4_ENABLE_FEX_GUEST_CPU
std::string DescribeGuestThreads() {
    return {};
}
#endif

} // namespace Core::GuestCpu
