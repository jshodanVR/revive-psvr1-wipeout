// SPDX-License-Identifier: MIT

#include "fex_hle_bridge.h"
#include "guest_watchdog.h"
#include "hle_trace.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <mutex>
#include <string>

#include <sys/syscall.h>
#include <unistd.h>

namespace {
// Diagnostic cap: high enough to capture full boot + background-thread HLE activity
// (the 256 default truncated the trace before the game's Fios driver thread ran).
constexpr uint32_t HleTraceLimit = 100000;
std::atomic_uint32_t HleTraceCount{};

thread_local Core::GuestCpu::HleCallFrame* ActiveHleCallFrame{};

class ActiveHleCallScope final {
public:
    explicit ActiveHleCallScope(Core::GuestCpu::HleCallFrame& frame)
        : previous{ActiveHleCallFrame} {
        ActiveHleCallFrame = &frame;
    }

    ~ActiveHleCallScope() {
        ActiveHleCallFrame = previous;
    }

private:
    Core::GuestCpu::HleCallFrame* previous;
};

// What each guest thread was last seen doing, for telling where a hung title is stuck. Guest
// threads spend their waits inside HLE calls (locks, events, file reads), so the call a thread
// is in and the guest code that made it describe a deadlock well.
struct ThreadActivity final {
    std::atomic<int> tid{};
    std::atomic<u64> operation{}; ///< call in progress, 0 while running guest code
    std::atomic<u64> last_operation{};
    std::atomic<u64> rsp{};
    std::atomic<u64> rbp{};
    std::atomic<u64> since_ms{}; ///< when the call in progress began, or the last one ended
};

constexpr std::size_t MaxTrackedThreads = 512;
std::array<ThreadActivity, MaxTrackedThreads> Activities;
std::atomic<std::size_t> NumActivities{};
thread_local ThreadActivity* MyActivity{};
std::atomic<Core::GuestCpu::HleCallRegistry*> ActivityNames{};

u64 CoarseMilliseconds() {
    timespec time{};
    clock_gettime(CLOCK_MONOTONIC_COARSE, &time);
    return static_cast<u64>(time.tv_sec) * 1000 + static_cast<u64>(time.tv_nsec) / 1'000'000;
}

ThreadActivity* ClaimActivity() {
    const auto index = NumActivities.fetch_add(1, std::memory_order_relaxed);
    if (index >= MaxTrackedThreads) {
        NumActivities.store(MaxTrackedThreads, std::memory_order_relaxed);
        return nullptr;
    }
    Activities[index].tid.store(static_cast<int>(syscall(SYS_gettid)), std::memory_order_relaxed);
    return &Activities[index];
}

class ActivityScope final {
public:
    explicit ActivityScope(const Core::GuestCpu::HleCallFrame& frame) {
        if (MyActivity == nullptr) {
            MyActivity = ClaimActivity();
        }
        activity = MyActivity;
        if (activity == nullptr) {
            return;
        }
        // HLE code calls back into the guest, which makes HLE calls of its own.
        outer_operation = activity->operation.load(std::memory_order_relaxed);
        outer_rsp = activity->rsp.load(std::memory_order_relaxed);
        outer_rbp = activity->rbp.load(std::memory_order_relaxed);
        activity->rsp.store(frame.rsp, std::memory_order_relaxed);
        activity->rbp.store(frame.gpr[5], std::memory_order_relaxed);
        activity->since_ms.store(CoarseMilliseconds(), std::memory_order_relaxed);
        activity->operation.store(frame.operation, std::memory_order_relaxed);
    }

    ~ActivityScope() {
        if (activity == nullptr) {
            return;
        }
        activity->last_operation.store(activity->operation.load(std::memory_order_relaxed),
                                       std::memory_order_relaxed);
        activity->rsp.store(outer_rsp, std::memory_order_relaxed);
        activity->rbp.store(outer_rbp, std::memory_order_relaxed);
        activity->since_ms.store(CoarseMilliseconds(), std::memory_order_relaxed);
        activity->operation.store(outer_operation, std::memory_order_relaxed);
    }

private:
    ThreadActivity* activity{};
    u64 outer_operation{};
    u64 outer_rsp{};
    u64 outer_rbp{};
};

/// Reads a word of another thread's stack without faulting if that stack is gone: the kernel
/// does the copy, a bad address is just an error.
bool ReadWord(u64 address, u64* value) {
    static int pipe_ends[2] = {-1, -1};
    static std::mutex pipe_mutex;
    std::scoped_lock lock{pipe_mutex};
    if (pipe_ends[0] < 0 && pipe(pipe_ends) != 0) {
        return false;
    }
    if (write(pipe_ends[1], reinterpret_cast<const void*>(address), sizeof(*value)) !=
        static_cast<ssize_t>(sizeof(*value))) {
        return false;
    }
    return read(pipe_ends[0], value, sizeof(*value)) == static_cast<ssize_t>(sizeof(*value));
}

std::string ThreadName(int tid) {
    char path[64];
    std::snprintf(path, sizeof(path), "/proc/self/task/%d/comm", tid);
    char name[64] = "gone";
    if (std::FILE* file = std::fopen(path, "r")) {
        if (std::fgets(name, sizeof(name), file) != nullptr) {
            name[std::strcspn(name, "\n")] = '\0';
        }
        std::fclose(file);
    }
    return name;
}
} // namespace

namespace Core::GuestCpu {

bool PublishHostRange(const void* pointer, std::size_t size, bool writable) {
    if (ActiveHleCallFrame == nullptr || pointer == nullptr ||
        ActiveHleCallFrame->publish_host_range == nullptr) {
        return false;
    }
    return ActiveHleCallFrame->publish_host_range(
        ActiveHleCallFrame->host_range_context, reinterpret_cast<std::uintptr_t>(pointer), size,
        writable);
}

bool RevokeHostRange(const void* pointer) {
    if (ActiveHleCallFrame == nullptr || pointer == nullptr ||
        ActiveHleCallFrame->revoke_host_range == nullptr) {
        return false;
    }
    return ActiveHleCallFrame->revoke_host_range(ActiveHleCallFrame->host_range_context,
                                                  reinterpret_cast<std::uintptr_t>(pointer));
}

HleGuestBridge::HleGuestBridge(HleCallRegistry& registry_, RangeValidator validator_,
                               void* validator_context_, FailureReporter reporter_,
                               void* reporter_context_, ExecutableRangeQuery executable_query_,
                               void* executable_query_context_)
    : registry{registry_}, validator{validator_}, validator_context{validator_context_},
      reporter{reporter_}, reporter_context{reporter_context_},
      executable_query{executable_query_}, executable_query_context{executable_query_context_} {
    ActivityNames.store(&registry, std::memory_order_relaxed);
}

std::string DescribeGuestThreads() {
    const auto name_of = [](u64 operation) -> std::string {
        auto* const names = ActivityNames.load(std::memory_order_relaxed);
        const auto adapter = names != nullptr ? names->Find(operation) : nullptr;
        return adapter != nullptr ? std::string{adapter->Name()} : std::to_string(operation);
    };

    std::string text;
    const u64 now = CoarseMilliseconds();
    const auto count = std::min(NumActivities.load(std::memory_order_relaxed), MaxTrackedThreads);
    for (std::size_t index = 0; index < count; ++index) {
        const auto& activity = Activities[index];
        const int tid = activity.tid.load(std::memory_order_relaxed);
        const auto name = ThreadName(tid);
        if (name == "gone") {
            continue;
        }
        const u64 operation = activity.operation.load(std::memory_order_relaxed);
        // The clock is coarse and the thread keeps running: its time may be ahead of ours.
        const u64 since = activity.since_ms.load(std::memory_order_relaxed);
        const u64 elapsed = now > since ? now - since : 0;
        char line[512];
        if (operation == 0) {
            std::snprintf(line, sizeof(line), "  %-20s tid %d: guest code for %llu ms, after %s\n",
                          name.c_str(), tid, static_cast<unsigned long long>(elapsed),
                          name_of(activity.last_operation.load(std::memory_order_relaxed)).c_str());
            text += line;
            continue;
        }
        std::snprintf(line, sizeof(line), "  %-20s tid %d: in %s for %llu ms, callers",
                      name.c_str(), tid, name_of(operation).c_str(),
                      static_cast<unsigned long long>(elapsed));
        text += line;
        // The veneer was entered by a call, so the stack pointer is at the return address; from
        // there on the frame pointers lead up the guest's stack.
        u64 return_address = 0;
        if (ReadWord(activity.rsp.load(std::memory_order_relaxed), &return_address)) {
            std::snprintf(line, sizeof(line), " %#llx",
                          static_cast<unsigned long long>(return_address));
            text += line;
        }
        u64 frame = activity.rbp.load(std::memory_order_relaxed);
        for (int depth = 0; depth < 12 && frame != 0 && (frame & 7) == 0; ++depth) {
            u64 next = 0;
            if (!ReadWord(frame, &next) || !ReadWord(frame + 8, &return_address)) {
                break;
            }
            std::snprintf(line, sizeof(line), " %#llx",
                          static_cast<unsigned long long>(return_address));
            text += line;
            if (next <= frame) {
                break;
            }
            frame = next;
        }
        text += "\n";
    }
    return text;
}

Fex::EngineResult<bool> HleGuestBridge::Invoke(HleCallFrame& frame) {
    const HleCallAdapter* adapter = registry.FindForCall(frame.operation);
    std::shared_ptr<HleCallAdapter> held;
    if (adapter == nullptr) {
        // Beyond the table: the slow way.
        held = registry.Find(frame.operation);
        adapter = held.get();
    }
    if (adapter == nullptr) {
        const HleCallFailure failure{ENOSYS, "unregistered HLE operation"};
        Report(failure);
        return Fex::EngineFailure{Fex::EngineStage::Bridge, failure.error};
    }
    // Tracing is opt-in (BACHATA_FEX_HLE_TRACE) and bounded. Default-on fprintf
    // on memcpy/lock HLE cut Driveclub 3D to 11-15 fps. After the window fills,
    // skip the fetch_add so hot polling APIs do not contend on the counter.
    static const bool trace_requested = BachataHleTraceRequested(std::getenv("BACHATA_FEX_HLE_TRACE"));
    auto trace_index = HleTraceCount.load(std::memory_order_relaxed);
    if (trace_requested && trace_index < HleTraceLimit) {
        trace_index = HleTraceCount.fetch_add(1, std::memory_order_relaxed);
    }
    const bool trace = trace_requested && trace_index < HleTraceLimit;
    if (trace) {
        std::fprintf(stderr,
                     "BACHATA_FEX_HLE_BEGIN index=%u operation=%llu name=%.*s rsp=%#llx\n",
                     trace_index, static_cast<unsigned long long>(frame.operation),
                     static_cast<int>(adapter->Name().size()), adapter->Name().data(),
                     static_cast<unsigned long long>(frame.rsp));
    }
    frame.validate_range = ValidateRange;
    frame.validate_context = this;
    frame.publish_host_range = PublishHostRange;
    frame.revoke_host_range = RevokeHostRange;
    frame.host_range_context = this;
    const ActiveHleCallScope active_frame{frame};
    const ActivityScope activity{frame};
    const auto result = adapter->Invoke(frame);
    if (const auto* failure = std::get_if<HleCallFailure>(&result)) {
        if (trace) {
            std::fprintf(stderr,
                         "BACHATA_FEX_HLE_END index=%u operation=%llu name=%.*s error=%d\n",
                         trace_index, static_cast<unsigned long long>(frame.operation),
                         static_cast<int>(adapter->Name().size()), adapter->Name().data(),
                         failure->error);
        }
        Report(*failure);
        return Fex::EngineFailure{Fex::EngineStage::Bridge, failure->error};
    }
    if (trace) {
        std::fprintf(stderr,
                     "BACHATA_FEX_HLE_END index=%u operation=%llu name=%.*s rax=%#llx\n",
                     trace_index, static_cast<unsigned long long>(frame.operation),
                     static_cast<int>(adapter->Name().size()), adapter->Name().data(),
                     static_cast<unsigned long long>(frame.gpr[0]));
    }
    return true;
}

std::optional<GuestExecutionRange> HleGuestBridge::QueryExecutableRange(
    std::uintptr_t address) {
    if (executable_query == nullptr) {
        return std::nullopt;
    }
    return executable_query(executable_query_context, address);
}

bool HleGuestBridge::ValidateRange(void* context, std::uintptr_t address, std::size_t size,
                                   bool writable) {
    if (context == nullptr) {
        return false;
    }
    const auto* bridge = static_cast<const HleGuestBridge*>(context);
    if (bridge->ValidatePublishedHostRange(address, size, writable)) {
        return true;
    }
    return bridge->validator != nullptr &&
           bridge->validator(bridge->validator_context, address, size, writable);
}

bool HleGuestBridge::PublishHostRange(void* context, std::uintptr_t address, std::size_t size,
                                      bool writable) {
    if (context == nullptr || address == 0 || size == 0 || address > UINTPTR_MAX - size) {
        return false;
    }
    auto* bridge = static_cast<HleGuestBridge*>(context);
    const HostRange range{address, address + size, writable};
    std::unique_lock lock{bridge->host_range_mutex};
    const bool overlaps = std::ranges::any_of(bridge->host_ranges, [&](const HostRange& existing) {
        return range.begin < existing.end && existing.begin < range.end;
    });
    if (overlaps) {
        return false;
    }
    bridge->host_ranges.push_back(range);
    return true;
}

bool HleGuestBridge::RevokeHostRange(void* context, std::uintptr_t address) {
    if (context == nullptr || address == 0) {
        return false;
    }
    auto* bridge = static_cast<HleGuestBridge*>(context);
    std::unique_lock lock{bridge->host_range_mutex};
    const auto range = std::ranges::find(bridge->host_ranges, address, &HostRange::begin);
    if (range == bridge->host_ranges.end()) {
        return false;
    }
    bridge->host_ranges.erase(range);
    return true;
}

bool HleGuestBridge::ValidatePublishedHostRange(std::uintptr_t address, std::size_t size,
                                                bool writable) const {
    if (address == 0 || size == 0 || address > UINTPTR_MAX - size) {
        return false;
    }
    const auto end = address + size;
    std::shared_lock lock{host_range_mutex};
    return std::ranges::any_of(host_ranges, [&](const HostRange& range) {
        return range.begin <= address && end <= range.end && (!writable || range.writable);
    });
}

void HleGuestBridge::Report(const HleCallFailure& failure) const {
    if (reporter != nullptr) {
        reporter(reporter_context, failure);
    }
}

} // namespace Core::GuestCpu
