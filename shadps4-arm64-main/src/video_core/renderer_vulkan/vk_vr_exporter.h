// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <memory>
#include <vector>

#include "common/types.h"
#include "core/vr/vr_runtime.h"
#include "video_core/renderer_vulkan/vk_common.h"

namespace Core::Vr {
struct HostBuffers;
}

namespace Vulkan {

class Instance;
class Scheduler;
struct Frame;

/// Delivers the frames of the emulated headset to the application that drives the real one.
/// That application owns the images. Where the driver can attach to them the frames are rendered
/// straight into them, otherwise they are rendered here and their pixels copied over.
class VrExporter {
public:
    VrExporter(const Instance& instance, Scheduler& scheduler);
    ~VrExporter();

    /// Returns the frame the next headset image has to be rendered into, or null when there is
    /// no host to deliver it to (yet).
    Frame* Acquire(vk::Format format);

    /// The same for the headset of the machine the emulator runs on (Core::Vr::OpenXrHost): a
    /// frame of `width` by `height` in that host's format, or null while that headset shows
    /// nothing. Such a frame is drawn next to the one for the window, not in place of it.
    Frame* AcquireLocal(u32 width, u32 height);
    /// Whether frames can be had from AcquireLocal at times.
    bool HasLocalHost() const;

    /// Records whatever gets the rendered frame into the host's image. Call after its draws.
    void Finalize(Frame* frame, vk::CommandBuffer cmdbuf);

    /// Tells the host the frame is ready. Blocks until the GPU is done with it.
    void Deliver(Frame* frame, const Core::Vr::PresentedFrame& info);

private:
    struct Slot;

    bool Setup(vk::Format format);
    bool Import(Slot& slot, const Core::Vr::HostBuffers& offer, vk::Format format);
    bool CreateCopy(Slot& slot, const Core::Vr::HostBuffers& offer, vk::Format format);
    void Destroy(Slot& slot);

    bool IsLocal(const Frame* frame) const;

    const Instance& instance;
    Scheduler& scheduler;
    /// Frames standing for the images of the machine's own headset host, by its index.
    std::vector<std::unique_ptr<Frame>> local_frames;
    std::vector<std::unique_ptr<Slot>> slots;
    u32 next_slot{};
    u32 stride{};
    bool swap_red_blue{};
    bool failed{};
};

} // namespace Vulkan
