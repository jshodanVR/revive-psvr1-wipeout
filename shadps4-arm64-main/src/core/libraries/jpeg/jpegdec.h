// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "common/types.h"

namespace Core::Loader {
class SymbolsResolver;
}

namespace Libraries::JpegDec {

enum class OrbisJpegDecColorSpace : u16 {
    Unknown = 0,
    Grayscale = 1,
    Ycc = 2,
    Rgb = 3,
};

enum class OrbisJpegDecPixelFormat : u16 {
    R8G8B8A8 = 0,
    B8G8R8A8 = 1,
};

struct OrbisJpegDecParseParam {
    const u8* jpeg_mem_addr;
    u32 jpeg_mem_size;
    u32 reserved;
};

struct OrbisJpegDecImageInfo {
    u32 image_width;
    u32 image_height;
    OrbisJpegDecColorSpace color_space;
    u16 bit_depth;
    u32 image_flag;
};

struct OrbisJpegDecCreateParam {
    u32 this_size;
    u32 attribute;
    u32 max_image_width;
};

using OrbisJpegDecHandle = void*;

struct OrbisJpegDecDecodeParam {
    const u8* jpeg_mem_addr;
    u8* image_mem_addr;
    u32 jpeg_mem_size;
    u32 image_mem_size;
    OrbisJpegDecPixelFormat pixel_format;
    u16 alpha_value;
    u32 image_pitch;
};

s32 PS4_SYSV_ABI sceJpegDecCreate(const OrbisJpegDecCreateParam* param, void* memory_address,
                                  u32 memory_size, OrbisJpegDecHandle* handle);
s32 PS4_SYSV_ABI sceJpegDecDecode(OrbisJpegDecHandle handle, const OrbisJpegDecDecodeParam* param,
                                  OrbisJpegDecImageInfo* image_info);
s32 PS4_SYSV_ABI sceJpegDecDecodeWithInputControl();
s32 PS4_SYSV_ABI sceJpegDecDelete(OrbisJpegDecHandle handle);
s32 PS4_SYSV_ABI sceJpegDecParseHeader(const OrbisJpegDecParseParam* param,
                                       OrbisJpegDecImageInfo* image_info);
s32 PS4_SYSV_ABI sceJpegDecQueryMemorySize(const OrbisJpegDecCreateParam* param);

void RegisterLib(Core::Loader::SymbolsResolver* sym);

} // namespace Libraries::JpegDec
