// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <array>
#include <cstring>
#include <memory>
#include "common/assert.h"
#include "core/libraries/kernel/process.h"
#include "core/libraries/videoout/buffer.h"
#include "shader_recompiler/resource.h"
#include "video_core/renderer_vulkan/liverpool_to_vk.h"
#include "video_core/texture_cache/host_compatibility.h"
#include "video_core/texture_cache/image_info.h"
#include "video_core/texture_cache/tile.h"

#include <magic_enum/magic_enum.hpp>

namespace VideoCore {

using namespace Vulkan;
using Libraries::VideoOut::TilingMode;
using VideoOutFormat = Libraries::VideoOut::PixelFormat;

static vk::Format ConvertPixelFormat(const VideoOutFormat format) {
    switch (format) {
    case VideoOutFormat::A8B8G8R8Srgb:
    // Remaining formats are mapped to RGBA for internal consistency and changed to BGRA in the
    // frame image view.
    case VideoOutFormat::A8R8G8B8Srgb:
        return vk::Format::eR8G8B8A8Srgb;
    case VideoOutFormat::A2R10G10B10:
    case VideoOutFormat::A2R10G10B10Srgb:
    case VideoOutFormat::A2R10G10B10Bt2020Pq:
        return vk::Format::eA2B10G10R10UnormPack32;
    default:
        break;
    }
    UNREACHABLE_MSG("Unknown format={}", static_cast<u32>(format));
    return {};
}

ImageInfo::ImageInfo(const Libraries::VideoOut::BufferAttributeGroup& group,
                     VAddr cpu_address) noexcept {
    const auto& attrib = group.attrib;
    props.is_tiled = attrib.tiling_mode == TilingMode::Tile;
    tile_mode =
        props.is_tiled ? AmdGpu::TileMode::Display2DThin : AmdGpu::TileMode::DisplayLinearAligned;
    array_mode = AmdGpu::GetArrayMode(tile_mode);
    pixel_format = ConvertPixelFormat(attrib.pixel_format);
    type = AmdGpu::ImageType::Color2D;
    size.width = attrib.width;
    size.height = attrib.height;
    pitch = attrib.tiling_mode == TilingMode::Linear ? size.width : (size.width + 127) & (~127);
    num_bits = attrib.pixel_format != VideoOutFormat::A16R16G16B16Float ? 32 : 64;
    ASSERT(num_bits == 32);

    guest_address = cpu_address;
    UpdateSize();
}

ImageInfo::ImageInfo(const AmdGpu::ColorBuffer& buffer, AmdGpu::CbDbExtent hint) noexcept {
    props.is_tiled = buffer.IsTiled();
    tile_mode = buffer.GetTileMode();
    array_mode = AmdGpu::GetArrayMode(tile_mode);
    pixel_format = LiverpoolToVK::SurfaceFormat(buffer.GetDataFmt(), buffer.GetNumberFmt());
    num_samples = buffer.NumSamples();
    num_bits = NumBitsPerBlock(buffer.GetDataFmt());
    type = AmdGpu::ImageType::Color2D;
    size.width = hint.Valid() ? hint.width : buffer.Pitch();
    size.height = hint.Valid() ? hint.height : buffer.Height();
    size.depth = 1;
    pitch = buffer.Pitch();
    resources.layers = buffer.NumSlices();
    meta_info.cmask_addr = buffer.info.fast_clear ? buffer.CmaskAddress() : 0;
    meta_info.fmask_addr = buffer.info.compression ? buffer.FmaskAddress() : 0;

    guest_address = buffer.Address();
    if (props.is_tiled) {
        guest_size = buffer.GetColorSliceSize() * resources.layers;
        mips_layout[0] = MipInfo(guest_size, pitch, buffer.Height(), 0);
    } else {
        std::tie(std::ignore, std::ignore, guest_size) =
            ImageSizeLinearAligned(pitch, size.height, num_bits, num_samples);
        guest_size *= resources.layers;
        mips_layout[0] = MipInfo(guest_size, pitch, size.height, 0);
    }
    alt_tile = Libraries::Kernel::sceKernelIsNeoMode() && buffer.info.alt_tile_mode;
}

ImageInfo::ImageInfo(const AmdGpu::DepthBuffer& buffer, u32 num_slices, VAddr htile_address,
                     AmdGpu::CbDbExtent hint, bool write_buffer) noexcept {
    tile_mode = buffer.GetTileMode();
    array_mode = AmdGpu::GetArrayMode(tile_mode);
    pixel_format = LiverpoolToVK::DepthFormat(buffer.z_info.format, buffer.stencil_info.format);
    type = AmdGpu::ImageType::Color2D;
    props.is_tiled = buffer.IsTiled();
    props.is_depth = true;
    props.has_stencil = buffer.stencil_info.format != AmdGpu::DepthBuffer::StencilFormat::Invalid;
    num_samples = buffer.NumSamples();
    num_bits = buffer.NumBits();
    size.width = hint.Valid() ? hint.width : buffer.Pitch();
    size.height = hint.Valid() ? hint.height : buffer.Height();
    size.depth = 1;
    pitch = buffer.Pitch();
    resources.layers = num_slices;
    meta_info.htile_addr = buffer.z_info.tile_surface_enable ? htile_address : 0;

    stencil_addr = write_buffer ? buffer.StencilWriteAddress() : buffer.StencilAddress();

    guest_address = write_buffer ? buffer.DepthWriteAddress() : buffer.DepthAddress();
    if (props.is_tiled) {
        guest_size = buffer.GetDepthSliceSize() * resources.layers;
        mips_layout[0] = MipInfo(guest_size, pitch, buffer.Height(), 0);
    } else {
        std::tie(std::ignore, std::ignore, guest_size) =
            ImageSizeLinearAligned(pitch, size.height, num_bits, num_samples);
        guest_size *= resources.layers;
        mips_layout[0] = MipInfo(guest_size, pitch, size.height, 0);
    }
    // One stencil byte per sample, including tiled padding and every layer.
    // See upstream shadPS4 #5296.
    stencil_size = guest_size / (num_bits >> 3);
}

namespace {

/// What a texture descriptor was worked out to describe. Every draw describes each of its
/// textures anew, and the layout of all mip levels in guest memory is part of the answer:
/// the same few hundred descriptors, thousands of times a frame.
struct DescribedTexture {
    std::array<u64, 4> sharp{};
    u32 traits{};
    bool valid{};
    ImageInfo info;
};

constexpr size_t NumDescribedTextures = 1024;

std::array<DescribedTexture, NumDescribedTextures>& DescribedTextures() {
    static thread_local std::unique_ptr<std::array<DescribedTexture, NumDescribedTextures>>
        entries;
    if (!entries) {
        entries = std::make_unique<std::array<DescribedTexture, NumDescribedTextures>>();
    }
    return *entries;
}

} // namespace

ImageInfo::ImageInfo(const AmdGpu::Image& image, const Shader::ImageResource& desc) noexcept {
    static_assert(sizeof(AmdGpu::Image) == sizeof(std::array<u64, 4>));
    std::array<u64, 4> sharp;
    std::memcpy(sharp.data(), &image, sizeof(sharp));
    // Everything else the result depends on.
    const u32 traits = (desc.is_depth ? 1u : 0u) | (desc.Is1DHostedAs2D(image) ? 2u : 0u);
    const u64 hash = (sharp[0] * 0x9E3779B97F4A7C15ULL) ^ (sharp[1] * 0xC2B2AE3D27D4EB4FULL) ^
                     (sharp[2] * 0x165667B19E3779F9ULL) ^ (sharp[3] + traits);
    auto& entry = DescribedTextures()[(hash ^ (hash >> 29)) % NumDescribedTextures];
    if (entry.valid && entry.sharp == sharp && entry.traits == traits) {
        *this = entry.info;
        return;
    }

    tile_mode = image.GetTileMode();
    array_mode = AmdGpu::GetArrayMode(tile_mode);
    pixel_format = LiverpoolToVK::SurfaceFormat(image.GetDataFmt(), image.GetNumberFmt());
    if (desc.is_depth) {
        pixel_format = LiverpoolToVK::PromoteFormatToDepth(pixel_format);
        props.is_depth = true;
    }
    type = desc.Is1DHostedAs2D(image) ? AmdGpu::ImageType::Color2D : image.GetBaseType();
    props.is_tiled = image.IsTiled();
    props.is_volume = type == AmdGpu::ImageType::Color3D;
    props.is_pow2 = image.pow2pad;
    props.is_block = AmdGpu::IsBlockCoded(image.GetDataFmt());
    size.width = image.width + 1;
    size.height = image.height + 1;
    size.depth = props.is_volume ? image.depth + 1 : 1;
    pitch = image.Pitch();
    resources.levels = image.NumLevels();
    resources.layers = image.NumLayers();
    num_samples = image.NumSamples();
    num_bits = NumBitsPerBlock(image.GetDataFmt());
    bank_swizzle = image.GetBankSwizzle();

    guest_address = image.Address();

    alt_tile = Libraries::Kernel::sceKernelIsNeoMode() && image.alt_tile_mode;
    UpdateSize();

    entry.sharp = sharp;
    entry.traits = traits;
    entry.info = *this;
    entry.valid = true;
}

bool ImageInfo::IsCompatible(const ImageInfo& info) const {
    return (IsVulkanFormatCompatible(pixel_format, info.pixel_format) ||
            IsVulkanFormatCompatible(info.pixel_format, pixel_format)) &&
           num_samples == info.num_samples && num_bits == info.num_bits;
}

void ImageInfo::UpdateSize() {
    guest_size = 0;
    for (s32 mip = 0; mip < resources.levels; ++mip) {
        u32 mip_w = pitch >> mip;
        u32 mip_h = size.height >> mip;
        if (props.is_block) {
            mip_w = (mip_w + 3) / 4;
            mip_h = (mip_h + 3) / 4;
        }
        mip_w = std::max(mip_w, 1u);
        mip_h = std::max(mip_h, 1u);
        u32 mip_d = std::max(size.depth >> mip, 1u);
        u32 thickness = 1;

        if (props.is_pow2) {
            mip_w = std::bit_ceil(mip_w);
            mip_h = std::bit_ceil(mip_h);
            mip_d = std::bit_ceil(mip_d);
        }

        auto& mip_info = mips_layout[mip];
        switch (array_mode) {
        case AmdGpu::ArrayMode::ArrayLinearAligned: {
            std::tie(mip_info.pitch, mip_info.height, mip_info.size) =
                ImageSizeLinearAligned(mip_w, mip_h, num_bits, num_samples);
            break;
        }
        case AmdGpu::ArrayMode::Array1DTiledThick:
            thickness = 4;
            mip_d += (-mip_d) & (thickness - 1);
            [[fallthrough]];
        case AmdGpu::ArrayMode::Array1DTiledThin1: {
            std::tie(mip_info.pitch, mip_info.height, mip_info.size) =
                ImageSizeMicroTiled(mip_w, mip_h, thickness, num_bits, num_samples);
            break;
        }
        case AmdGpu::ArrayMode::Array2DTiledThick:
            thickness = 4;
            mip_d += (-mip_d) & (thickness - 1);
            [[fallthrough]];
        case AmdGpu::ArrayMode::Array2DTiledThin1: {
            ASSERT(!props.is_block);
            std::tie(mip_info.pitch, mip_info.height, mip_info.size) = ImageSizeMacroTiled(
                mip_w, mip_h, thickness, num_bits, num_samples, tile_mode, mip, alt_tile);
            break;
        }
        default: {
            UNREACHABLE_MSG("Unknown array mode {}", magic_enum::enum_name(array_mode));
        }
        }
        if (props.is_block) {
            mip_info.pitch = std::max(mip_info.pitch * 4, 32u);
            mip_info.height = std::max(mip_info.height * 4, 32u);
        }
        mip_info.size *= mip_d * resources.layers;
        mip_info.offset = guest_size;
        guest_size += mip_info.size;
    }
}

s32 ImageInfo::MipOf(const ImageInfo& info) const {
    if (!IsCompatible(info)) {
        return -1;
    }

    if (info.array_mode != array_mode) {
        return -1;
    }

    // Currently we expect only on level to be copied.
    if (resources.levels != 1) {
        return -1;
    }

    // Find mip
    auto mip = -1;
    for (auto m = 0; m < info.resources.levels; ++m) {
        const auto& [mip_size, mip_pitch, mip_height, mip_ofs] = info.mips_layout[m];
        const VAddr mip_base = info.guest_address + mip_ofs;
        const VAddr mip_end = mip_base + mip_size;
        const u32 slice_size = mip_size / info.resources.layers;
        if (guest_address >= mip_base && guest_address < mip_end &&
            (guest_address - mip_base) % slice_size == 0) {
            mip = m;
            break;
        }
    }

    if (mip < 0) {
        return -1;
    }

    const auto curr_block_dim = BlockDim();
    const auto info_block_dim = info.BlockDim();

    // 2D block dimensions of both images should be the same.
    const auto mip_w = std::max(info_block_dim.width >> mip, 1u);
    const auto mip_h = std::max(info_block_dim.height >> mip, 1u);
    if ((curr_block_dim.width != mip_w) || (curr_block_dim.height != mip_h)) {
        // Small uncompressed render targets retain a tile-aligned pitch even when
        // their logical mip width is 4, 2, or 1. Compare that pitch with the parent's
        // stored mip layout, not with an unaligned right shift of its base pitch.
        const bool padded_mip = !props.is_block && !info.props.is_block &&
            size.width == std::max(info.size.width >> mip, 1u) &&
            size.height == std::max(info.size.height >> mip, 1u) &&
            pitch == info.mips_layout[mip].pitch;
        if (!padded_mip) {
            return -1;
        }
    }

    const auto mip_d = std::max(info.size.depth >> mip, 1u);
    if (info.type == AmdGpu::ImageType::Color3D && type == AmdGpu::ImageType::Color2D) {
        // In case of 2D array to 3D copy, make sure we have proper number of layers.
        if (resources.layers != mip_d) {
            return -1;
        }
    } else {
        if (type != info.type) {
            return -1;
        }
    }

    return mip;
}

s32 ImageInfo::SliceOf(const ImageInfo& info, s32 mip) const {
    if (mip < 0 || mip >= info.resources.levels || info.resources.layers == 0) {
        return -1;
    }
    if (!IsCompatible(info)) {
        return -1;
    }

    // Array slices should be of the same type.
    if (type != info.type) {
        return -1;
    }

    const auto curr_block_dim = BlockDim();
    const auto info_block_dim = info.BlockDim();

    // 2D block dimensions of both images should be the same.
    const auto mip_w = std::max(info_block_dim.width >> mip, 1u);
    const auto mip_h = std::max(info_block_dim.height >> mip, 1u);
    if ((curr_block_dim.width != mip_w) || (curr_block_dim.height != mip_h)) {
        // Small uncompressed render targets retain a tile-aligned pitch even when
        // their logical mip width is 4, 2, or 1. Compare that pitch with the parent's
        // stored mip layout, not with an unaligned right shift of its base pitch.
        const bool padded_mip = !props.is_block && !info.props.is_block &&
            size.width == std::max(info.size.width >> mip, 1u) &&
            size.height == std::max(info.size.height >> mip, 1u) &&
            pitch == info.mips_layout[mip].pitch;
        if (!padded_mip) {
            return -1;
        }
    }

    // A view can span several layers. Address alignment and the layer index
    // are measured in individual slices, not in the size of the entire view.
    const auto& layout = info.mips_layout[mip];
    const u32 slice_size = layout.size / info.resources.layers;
    if (slice_size == 0 || guest_size == 0 || guest_size % slice_size != 0) {
        return -1;
    }
    const VAddr mip_base = info.guest_address + layout.offset;
    if (guest_address < mip_base) {
        return -1;
    }
    const auto addr_diff = guest_address - mip_base;
    if (addr_diff >= layout.size || guest_size > layout.size - addr_diff ||
        addr_diff % slice_size != 0) {
        return -1;
    }
    return static_cast<s32>(addr_diff / slice_size);
}

} // namespace VideoCore
