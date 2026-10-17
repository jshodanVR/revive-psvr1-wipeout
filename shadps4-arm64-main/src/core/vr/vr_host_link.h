// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <thread>

#include "common/types.h"
#include "core/vr/vr_protocol.h"

namespace Core::Vr {

/// The images the host wants frames delivered in. The file descriptors belong to the receiver.
struct HostBuffers {
    std::array<int, Protocol::MaxBuffers> fds{-1, -1, -1, -1};
    u32 count{};
    u32 width{};
    u32 height{};
    u32 stride{};
};

/// Connection to the application that owns the real headset: head poses come in, finished
/// frames go out.
class HostLink {
public:
    static HostLink& Instance();

    /// Connects to the host named by SHADPS4_VR_SOCKET. Returns false when there is none.
    bool Start();

    bool IsConnected() const {
        return connected.load(std::memory_order_relaxed);
    }

    /// Waits for the host to offer its buffers. They are handed out once.
    std::optional<HostBuffers> TakeBuffers(std::chrono::milliseconds timeout);

    bool SendFrame(const Protocol::Frame& frame);
    bool SendPadFeedback(const Protocol::PadFeedback& feedback);

private:
    HostLink() = default;

    void ReadLoop();

    int fd{-1};
    std::mutex send_mutex;
    std::atomic<bool> connected{};
    std::thread reader;
    std::mutex mutex;
    std::condition_variable buffers_cv;
    std::optional<HostBuffers> buffers;
};

} // namespace Core::Vr
