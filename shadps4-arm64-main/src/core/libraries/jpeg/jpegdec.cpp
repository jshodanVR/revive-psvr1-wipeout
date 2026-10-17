// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#ifdef JPEGDEC_STANDALONE_TEST
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_JPEG
#define STBI_NO_STDIO
#include <stb_image.h>
#define LOG_ERROR(...) ((void)0)
#define LOG_TRACE(...) ((void)0)
#else
#include "common/logging/log.h"
#include "common/stb.h"
#include "core/libraries/libs.h"
#endif

#include "core/libraries/error_codes.h"
#include "core/libraries/jpeg/jpeg_error.h"
#include "core/libraries/jpeg/jpegdec.h"

namespace Libraries::JpegDec {

namespace {

constexpr u32 kHandleMagic = 0x4A504547; // 'JPEG'

struct JpegHandler {
    u32 magic;
};

bool IsValidHandle(OrbisJpegDecHandle handle) {
    const auto* jpegh = static_cast<const JpegHandler*>(handle);
    return jpegh != nullptr && jpegh->magic == kHandleMagic;
}

s32 FillImageInfo(const u8* data, u32 size, OrbisJpegDecImageInfo* image_info) {
    int width = 0;
    int height = 0;
    int components = 0;
    if (stbi_info_from_memory(data, static_cast<int>(size), &width, &height, &components) == 0) {
        return ORBIS_JPEG_DEC_ERROR_INVALID_DATA;
    }
    if (image_info != nullptr) {
        image_info->image_width = static_cast<u32>(width);
        image_info->image_height = static_cast<u32>(height);
        image_info->bit_depth = 8;
        image_info->image_flag = 0;
        image_info->color_space =
            components == 1 ? OrbisJpegDecColorSpace::Grayscale : OrbisJpegDecColorSpace::Ycc;
    }
    return ORBIS_OK;
}

} // namespace

s32 PS4_SYSV_ABI sceJpegDecCreate(const OrbisJpegDecCreateParam* param, void* memory_address,
                                  u32 memory_size, OrbisJpegDecHandle* handle) {
    if (param == nullptr || handle == nullptr) {
        LOG_ERROR(Lib_Jpeg, "Invalid param!");
        return ORBIS_JPEG_DEC_ERROR_INVALID_ADDR;
    }
    if (memory_address == nullptr) {
        LOG_ERROR(Lib_Jpeg, "Invalid memory address!");
        return ORBIS_JPEG_DEC_ERROR_INVALID_ADDR;
    }
    if (memory_size < sizeof(JpegHandler) || param->max_image_width - 1 > 1000000) {
        LOG_ERROR(Lib_Jpeg, "Invalid size! width = {} memory = {}", param->max_image_width,
                  memory_size);
        return ORBIS_JPEG_DEC_ERROR_INVALID_SIZE;
    }
    auto* jpegh = static_cast<JpegHandler*>(memory_address);
    jpegh->magic = kHandleMagic;
    *handle = jpegh;
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceJpegDecDecode(OrbisJpegDecHandle handle, const OrbisJpegDecDecodeParam* param,
                                  OrbisJpegDecImageInfo* image_info) {
    if (!IsValidHandle(handle)) {
        LOG_ERROR(Lib_Jpeg, "invalid handle!");
        return ORBIS_JPEG_DEC_ERROR_INVALID_HANDLE;
    }
    if (param == nullptr) {
        LOG_ERROR(Lib_Jpeg, "Invalid param!");
        return ORBIS_JPEG_DEC_ERROR_INVALID_PARAM;
    }
    if (param->jpeg_mem_addr == nullptr || param->image_mem_addr == nullptr) {
        LOG_ERROR(Lib_Jpeg, "invalid image address!");
        return ORBIS_JPEG_DEC_ERROR_INVALID_ADDR;
    }
    if (param->pixel_format != OrbisJpegDecPixelFormat::R8G8B8A8 &&
        param->pixel_format != OrbisJpegDecPixelFormat::B8G8R8A8) {
        LOG_ERROR(Lib_Jpeg, "unsupported pixel format {}", static_cast<u32>(param->pixel_format));
        return ORBIS_JPEG_DEC_ERROR_INVALID_PARAM;
    }

    int width = 0;
    int height = 0;
    int components = 0;
    u8* decoded = stbi_load_from_memory(param->jpeg_mem_addr, static_cast<int>(param->jpeg_mem_size),
                                        &width, &height, &components, 4);
    if (decoded == nullptr) {
        LOG_ERROR(Lib_Jpeg, "decode failed: {}", stbi_failure_reason());
        return ORBIS_JPEG_DEC_ERROR_INVALID_DATA;
    }

    if (image_info != nullptr) {
        image_info->image_width = static_cast<u32>(width);
        image_info->image_height = static_cast<u32>(height);
        image_info->bit_depth = 8;
        image_info->image_flag = 0;
        image_info->color_space =
            components == 1 ? OrbisJpegDecColorSpace::Grayscale : OrbisJpegDecColorSpace::Ycc;
    }

    const s32 stride = param->image_pitch > 0 ? static_cast<s32>(param->image_pitch) : width * 4;
    const u16 alpha = param->alpha_value;
    const bool swap_rb = param->pixel_format == OrbisJpegDecPixelFormat::B8G8R8A8;
    for (int y = 0; y < height; ++y) {
        auto* dst = param->image_mem_addr + static_cast<size_t>(y) * static_cast<size_t>(stride);
        const u8* src = decoded + static_cast<size_t>(y) * static_cast<size_t>(width) * 4;
        for (int x = 0; x < width; ++x) {
            const u8 r = src[x * 4 + 0];
            const u8 g = src[x * 4 + 1];
            const u8 b = src[x * 4 + 2];
            dst[x * 4 + 0] = swap_rb ? b : r;
            dst[x * 4 + 1] = g;
            dst[x * 4 + 2] = swap_rb ? r : b;
            dst[x * 4 + 3] = components == 1 || components == 3 ? static_cast<u8>(alpha) : src[x * 4 + 3];
        }
    }
    stbi_image_free(decoded);

    return (width > 32767 || height > 32767) ? 0 : (width << 16) | height;
}

s32 PS4_SYSV_ABI sceJpegDecDecodeWithInputControl() {
    LOG_ERROR(Lib_Jpeg, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceJpegDecDelete(OrbisJpegDecHandle handle) {
    if (!IsValidHandle(handle)) {
        LOG_ERROR(Lib_Jpeg, "invalid handle!");
        return ORBIS_JPEG_DEC_ERROR_INVALID_HANDLE;
    }
    static_cast<JpegHandler*>(handle)->magic = 0;
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceJpegDecParseHeader(const OrbisJpegDecParseParam* param,
                                       OrbisJpegDecImageInfo* image_info) {
    if (param == nullptr || image_info == nullptr) {
        LOG_ERROR(Lib_Jpeg, "Invalid param!");
        return ORBIS_JPEG_DEC_ERROR_INVALID_ADDR;
    }
    if (param->jpeg_mem_addr == nullptr || param->jpeg_mem_size == 0) {
        LOG_ERROR(Lib_Jpeg, "Invalid jpeg address!");
        return ORBIS_JPEG_DEC_ERROR_INVALID_ADDR;
    }
    return FillImageInfo(param->jpeg_mem_addr, param->jpeg_mem_size, image_info);
}

s32 PS4_SYSV_ABI sceJpegDecQueryMemorySize(const OrbisJpegDecCreateParam* param) {
    if (param == nullptr) {
        LOG_ERROR(Lib_Jpeg, "Invalid param!");
        return ORBIS_JPEG_DEC_ERROR_INVALID_ADDR;
    }
    if (param->max_image_width - 1 > 1000000) {
        LOG_ERROR(Lib_Jpeg, "Invalid size! width = {}", param->max_image_width);
        return ORBIS_JPEG_DEC_ERROR_INVALID_SIZE;
    }
    return static_cast<s32>(sizeof(JpegHandler));
}

#ifndef JPEGDEC_STANDALONE_TEST
void RegisterLib(Core::Loader::SymbolsResolver* sym) {
    LIB_FUNCTION("JPh3Zgg0Zwc", "libSceJpegDec", 1, "libSceJpegDec", sceJpegDecCreate);
    LIB_FUNCTION("1kzQRoWEgSA", "libSceJpegDec", 1, "libSceJpegDec", sceJpegDecDecode);
    LIB_FUNCTION("919MhccOiII", "libSceJpegDec", 1, "libSceJpegDec",
                 sceJpegDecDecodeWithInputControl);
    LIB_FUNCTION("Hwh11+m5KoI", "libSceJpegDec", 1, "libSceJpegDec", sceJpegDecDelete);
    LIB_FUNCTION("LSinoSQH790", "libSceJpegDec", 1, "libSceJpegDec", sceJpegDecParseHeader);
    LIB_FUNCTION("uNAUmANZMEw", "libSceJpegDec", 1, "libSceJpegDec", sceJpegDecQueryMemorySize);
}
#endif

} // namespace Libraries::JpegDec
