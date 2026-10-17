// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "common/types.h"
#include "core/vr/vr_runtime.h"
#include "video_core/renderer_vulkan/vk_common.h"

namespace Core::Vr {

/// The headset of the machine the emulator runs on, reached through that machine's OpenXR
/// runtime: on a PC whatever drives the headset there (Virtual Desktop, SteamVR, the Oculus
/// runtime). It plays the part the VR host application plays on a standalone headset
/// (vr_host_link.h), inside the emulator's own process: head and hand poses come in, the frames
/// of the emulated headset go out as a layer the runtime's compositor turns to where the head
/// points by the time it is shown - the reprojection a PlayStation VR does itself.
class OpenXrHost {
public:
    static OpenXrHost& Instance();

    /// Looks for an OpenXR runtime and, through it, for a headset. Has to happen before the
    /// renderer creates its Vulkan instance and device, which have to suit the runtime. Returns
    /// true when there is a runtime: the headset itself may turn up later.
    bool Connect();

    /// A runtime answered: frames are shown through it whenever it has a headset.
    bool IsAvailable() const;
    /// The runtime had a headset when it was asked at Connect.
    bool HasHeadset() const;

    /// What the runtime wants of the Vulkan instance and device the session is to draw with,
    /// beyond what the renderer enables for itself. Names the driver does not know are for the
    /// renderer to leave out.
    std::vector<std::string> VulkanInstanceExtensions() const;
    std::vector<std::string> VulkanDeviceExtensions() const;
    /// The graphics card the headset hangs off, if the runtime can say by now.
    vk::PhysicalDevice PreferredPhysicalDevice(vk::Instance instance) const;

    struct Graphics {
        vk::Instance instance;
        vk::PhysicalDevice physical_device;
        vk::Device device;
        vk::Queue queue;
        u32 queue_family{};
        u32 queue_index{};
    };
    /// Starts the thread that keeps the session with the headset going on the renderer's device.
    void Start(const Graphics& graphics);
    /// Ends the session, so that the headset's system knows the game is over before the
    /// process is gone. For when the emulator closes, while its device still exists.
    void Shutdown();

    /// The format frames are handed over in.
    static constexpr vk::Format FrameFormat = vk::Format::eR8G8B8A8Srgb;

    struct Target {
        u32 index{};
        vk::Image image;
        vk::ImageView view;
        u32 width{};
        u32 height{};
    };
    /// Where the next frame of the title is to be drawn, both eyes side by side: an image of
    /// `width` by `height` in FrameFormat. Nothing while no headset is showing frames. For the
    /// thread that draws.
    std::optional<Target> BeginFrame(u32 width, u32 height);
    /// The GPU has finished the frame drawn to target `index`, and its image has been left in
    /// the General layout.
    void EndFrame(u32 index, const PresentedFrame& info);
    /// The frame for target `index` will not be finished after all.
    void DropFrame(u32 index);

    /// Whether somebody is looking: the session is running and the runtime shows its frames.
    bool IsShowing() const;

    /// The name of the sound output and input of the headset as the system lists them, empty
    /// if the runtime does not say.
    std::string AudioOutputName() const;
    std::string AudioInputName() const;
    /// Sound devices have come or gone: the runtime is asked for the headset's again, shortly.
    /// (One that streams to a headset has its devices only while it does: asked before, it
    /// names whatever the system plays on then.)
    void AudioDevicesChanged();

private:
    OpenXrHost();
    ~OpenXrHost();

    struct Impl;
    std::unique_ptr<Impl> impl;
};

} // namespace Core::Vr
