//  SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
//  SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <optional>
#include <span>
#include <vector>

#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"
#include "video_core/renderer_vulkan/vk_resource_pool.h"

namespace Vulkan {
class Instance;
class Frame;
}

namespace Vulkan::HostPasses {

class PostProcessingPass {
public:
    struct Settings {
        float gamma = 1.0f;
        u32 hdr = 0;
        /// 0..1: how much the picture is sharpened on the way (host_shaders/post_process.frag).
        float sharpen = 0.0f;
        /// For an output image of an sRGB format, which does the encoding for display itself.
        u32 linear_out = 0;
        std::array<float, 2> uv_scale{1.0f, 1.0f};
        std::array<float, 2> uv_offset{};
        u32 diagnostic_eye = 0; // 0: game, 1: left label, 2: right label.
        u32 overlay_enabled = 0;
        std::array<float, 2> overlay_scale{};
        std::array<float, 2> overlay_offset{};
    };

    void Create(const Instance& instance, MasterSemaphore* master_semaphore,
                vk::Format surface_format);

    /// An input image and the part of the output frame it is drawn into.
    struct Region {
        vk::ImageView input;
        vk::Rect2D area;
        std::array<float, 2> uv_scale{1.0f, 1.0f};
        std::array<float, 2> uv_offset{};
        u32 diagnostic_eye = 0;
        std::optional<vk::Rect2D> clip;
        vk::ImageView overlay{};
        std::array<float, 2> overlay_scale{};
        std::array<float, 2> overlay_offset{};
        std::vector<vk::ClearRect> hidden_areas;
    };

    // Frame marker: a row of black and white blocks stamped along the top-left edge of a frame.
    // Two sync blocks (white, black) are followed by a 16 bit frame id and an 8 bit check value,
    // least significant bit first. It lets a host compositor recognise which emulated frame it
    // is looking at after the image has travelled through a window system.
    static constexpr u32 MarkerBlockSize = 8;
    static constexpr u32 MarkerSyncBlocks = 2;
    static constexpr u32 MarkerIdBits = 16;
    static constexpr u32 MarkerCheckBits = 8;
    static constexpr u32 MarkerBlocks = MarkerSyncBlocks + MarkerIdBits + MarkerCheckBits;
    static constexpr u32 MarkerCheck(u32 id) {
        return ((id & 0xff) ^ ((id >> 8) & 0xff) ^ 0x5a) & 0xff;
    }

    void Render(vk::CommandBuffer cmdbuf, vk::ImageView input, vk::Extent2D input_size,
                Frame& output, Settings settings);

    /// Draws every region into the output frame and optionally stamps a frame marker.
    void Render(vk::CommandBuffer cmdbuf, std::span<const Region> regions, Frame& output,
                Settings settings, std::optional<u32> marker = std::nullopt);

private:
    vk::Device device{};
    bool uses_push_descriptors{};
    // Pool sizes must outlive desc_heap (DescriptorHeap stores a span to it).
    static constexpr std::array<vk::DescriptorPoolSize, 1> pool_sizes{{
        {vk::DescriptorType::eCombinedImageSampler, 128},
    }};
    DescriptorHeap desc_heap;
    vk::UniquePipeline pipeline{};
    vk::UniquePipelineLayout pipeline_layout{};
    vk::UniqueDescriptorSetLayout desc_set_layout{};
    vk::UniqueSampler sampler{};
};

} // namespace Vulkan::HostPasses
