// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <atomic>
#include <mutex>
#include <cstdlib>
#include <string_view>
#include <set>
#include <utility>

#include "common/logging/log.h"
#include "common/elf_info.h"
#include "shader_recompiler/resource.h"
#include "video_core/renderer_vulkan/liverpool_to_vk.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/texture_cache/image.h"
#include "video_core/texture_cache/image_view.h"

#include <magic_enum/magic_enum.hpp>

namespace VideoCore {

vk::ImageViewType ConvertImageViewType(AmdGpu::ImageType type) {
    switch (type) {
    case AmdGpu::ImageType::Color1D:
        return vk::ImageViewType::e1D;
    case AmdGpu::ImageType::Color1DArray:
        return vk::ImageViewType::e1DArray;
    case AmdGpu::ImageType::Color2D:
    case AmdGpu::ImageType::Color2DMsaa:
        return vk::ImageViewType::e2D;
    case AmdGpu::ImageType::Color2DArray:
        return vk::ImageViewType::e2DArray;
    case AmdGpu::ImageType::Color3D:
        return vk::ImageViewType::e3D;
    default:
        UNREACHABLE();
    }
}

ImageViewInfo::ImageViewInfo(const AmdGpu::Image& image, const Shader::ImageResource& desc) noexcept
    : is_storage{desc.is_written} {
    const auto dfmt = image.GetDataFmt();
    auto nfmt = image.GetNumberFmt();
    if (is_storage && nfmt == AmdGpu::NumberFormat::Srgb) {
        nfmt = AmdGpu::NumberFormat::Unorm;
    }
    format = Vulkan::LiverpoolToVK::SurfaceFormat(dfmt, nfmt);
    if (desc.is_depth) {
        format = Vulkan::LiverpoolToVK::PromoteFormatToDepth(format);
    }

    range.base.level = image.base_level;
    range.base.layer = image.base_array;
    range.extent.levels = image.NumViewLevels(desc.is_array);
    range.extent.layers = image.NumViewLayers(desc.is_array);
    type = desc.GetHostViewType(image);

    if (!is_storage) {
        mapping = Vulkan::LiverpoolToVK::ComponentMapping(image.DstSelect());
        // Rush's streamed resources may keep their full
        // dimensions while forbidding access to its non-resident top mip levels.
        static const bool rush_residency = [] {
            const char* value = std::getenv("SHADPS4_TEST_RUSH_MIN_LOD");
            return (!value || std::string_view{value} != "0") &&
                   Common::ElfInfo::Instance().GameSerial() == "CUSA02350";
        }();
        if (rush_residency && image.NumSamples() == 1) {
            min_lod = static_cast<u16>(image.min_lod);
        }
    }
}

ImageViewInfo::ImageViewInfo(const AmdGpu::ColorBuffer& col_buffer) noexcept {
    range.base.layer = col_buffer.BaseSlice();
    range.extent.layers = col_buffer.NumSlices() - range.base.layer;
    type = range.extent.layers > 1 ? AmdGpu::ImageType::Color2DArray : AmdGpu::ImageType::Color2D;
    format =
        Vulkan::LiverpoolToVK::SurfaceFormat(col_buffer.GetDataFmt(), col_buffer.GetNumberFmt());
}

ImageViewInfo::ImageViewInfo(const AmdGpu::DepthBuffer& depth_buffer, AmdGpu::DepthView view,
                             AmdGpu::DepthControl ctl) {
    format = Vulkan::LiverpoolToVK::DepthFormat(depth_buffer.z_info.format,
                                                depth_buffer.stencil_info.format);
    is_storage = ctl.depth_write_enable;
    range.base.layer = view.slice_start;
    range.extent.layers = view.NumSlices() - range.base.layer;
    type = range.extent.layers > 1 ? AmdGpu::ImageType::Color2DArray : AmdGpu::ImageType::Color2D;
}

namespace {

/// When sampling D32/D16 texture from shader, the T# specifies R32/R16 format so adjust it.
std::pair<vk::Format, vk::ImageAspectFlags> ViewFormatAndAspect(const ImageViewInfo& info,
                                                                const Image& image) {
    vk::Format format = info.format;
    vk::ImageAspectFlags aspect = image.aspect_mask;
    if (image.aspect_mask & vk::ImageAspectFlagBits::eDepth &&
        Vulkan::LiverpoolToVK::IsFormatDepthCompatible(format)) {
        format = image.info.pixel_format;
        aspect = vk::ImageAspectFlagBits::eDepth;
    }
    if (image.aspect_mask & vk::ImageAspectFlagBits::eStencil &&
        Vulkan::LiverpoolToVK::IsFormatStencilCompatible(format)) {
        format = image.info.pixel_format;
        aspect = vk::ImageAspectFlagBits::eStencil;
    }
    return {format, aspect};
}

} // namespace

vk::Format ImageView::HostFormat(const Vulkan::Instance& instance, const ImageViewInfo& info,
                                 const Image& image) {
    return instance.GetSupportedFormat(ViewFormatAndAspect(info, image).first,
                                       image.format_features);
}

ImageView::ImageView(const Vulkan::Instance& instance, const ImageViewInfo& info_,
                     const Image& image)
    : info{info_} {
    // A multisampled backing image may have had to give up some usage.
    vk::ImageViewUsageCreateInfo usage_ci{.usage = image.usage_flags &
                                                   image.backing->image.image_ci.usage};
    if (!info.is_storage) {
        usage_ci.usage &= ~vk::ImageUsageFlagBits::eStorage;
    }
    const auto [format, aspect] = ViewFormatAndAspect(info, image);

    vk::ImageViewMinLodCreateInfoEXT min_lod_ci{};
    if (info.min_lod && instance.IsImageViewMinLodSupported() && !info.is_storage) {
        min_lod_ci.minLod = std::min(static_cast<float>(info.min_lod) / 256.0f,
            static_cast<float>(info.range.base.level + info.range.extent.levels - 1));
        usage_ci.pNext = &min_lod_ci;
        static std::atomic_uint32_t reports{0};
        if (reports.fetch_add(1, std::memory_order_relaxed) < 128) {
            LOG_INFO(Render_Vulkan, "RESOURCE_MIN_LOD image={:#x} min={} base={} levels={}",
                image.info.guest_address, min_lod_ci.minLod, info.range.base.level,
                info.range.extent.levels);
        }
    } else if (info.min_lod && !instance.IsImageViewMinLodSupported()) {
        static std::atomic_bool reported{false};
        if (!reported.exchange(true, std::memory_order_relaxed)) {
            LOG_WARNING(Render_Vulkan, "Rush texture residency requires VK_EXT_image_view_min_lod; "
                        "unavailable on this device. Streamed textures may be corrupt.");
        }
    }

    const vk::ImageViewCreateInfo image_view_ci = {
        .pNext = &usage_ci,
        .image = image.GetImage(),
        .viewType = ConvertImageViewType(info.type),
        .format = instance.GetSupportedFormat(format, image.format_features),
        .components = info.mapping,
        .subresourceRange{
            .aspectMask = aspect,
            .baseMipLevel = info.range.base.level,
            .levelCount = info.range.extent.levels,
            .baseArrayLayer = info.range.base.layer,
            .layerCount = info.range.extent.layers,
        },
    };
    if (!IsViewTypeCompatible(info.type, image.info.type)) {
        LOG_ERROR(Render_Vulkan, "image view type {} is incompatible with image type {}",
                  magic_enum::enum_name(info.type), magic_enum::enum_name(image.info.type));
    }
    if (image_view_ci.format != image.backing->image.image_ci.format) {
        // Which reinterpretations a title asks for decides what its images have to allow
        // (see Image::EnsureViewFormat).
        static std::mutex seen_mutex;
        static std::set<std::pair<vk::Format, vk::Format>> seen;
        std::scoped_lock lock{seen_mutex};
        if (seen.emplace(image.backing->image.image_ci.format, image_view_ci.format).second) {
            LOG_INFO(Render_Vulkan, "image of format {} viewed as {} (first seen on {}x{})",
                     vk::to_string(image.backing->image.image_ci.format),
                     vk::to_string(image_view_ci.format), image.info.size.width,
                     image.info.size.height);
        }
    }

    auto [view_result, view] = instance.GetDevice().createImageViewUnique(image_view_ci);
    ASSERT_MSG(view_result == vk::Result::eSuccess, "Failed to create image view: {}",
               vk::to_string(view_result));
    image_view = std::move(view);

    const auto view_aspect = aspect & vk::ImageAspectFlagBits::eDepth     ? "Depth"
                             : aspect & vk::ImageAspectFlagBits::eStencil ? "Stencil"
                                                                          : "Color";
    Vulkan::SetObjectName(
        instance.GetDevice(), *image_view, "ImageView {}x{}x{} {:#x}:{:#x} {}:{} {}:{} ({})",
        image.info.size.width, image.info.size.height, image.info.size.depth,
        image.info.guest_address, image.info.guest_size, info.range.base.level,
        info.range.base.level + info.range.extent.levels - 1, info.range.base.layer,
        info.range.base.layer + info.range.extent.layers - 1, view_aspect);
}

ImageView::~ImageView() = default;

} // namespace VideoCore
