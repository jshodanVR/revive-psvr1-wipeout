// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <algorithm>
#include <cstring>

#include "common/types.h"

namespace Libraries::AvPlayer {

// Driveclub's guest AvPlayer file callback is not safe at FFmpeg's 64 KiB AVIO
// size: a 64 KiB guest allocate plus one read NULL-derefs on AvDemuxer (~1s in).
inline constexpr u32 kAvPlayerGuestReadChunkMax = 4096;

inline u32 AvPlayerGuestReadChunk(u32 requested) {
    return std::min(requested, kAvPlayerGuestReadChunkMax);
}

inline u32 AvPlayerGuestReadChunkCount(u32 length) {
    if (length == 0) {
        return 0;
    }
    return (length + kAvPlayerGuestReadChunkMax - 1) / kAvPlayerGuestReadChunkMax;
}

enum class AvPlayerReadStatus {
    Ok,
    Eof,
    IoError,
};

inline AvPlayerReadStatus AvPlayerClassifyRead(s32 bytes_read, u32 requested, u64 position,
                                               u64 file_size) {
    if (bytes_read > 0) {
        return AvPlayerReadStatus::Ok;
    }
    if (requested == 0 || position >= file_size) {
        return AvPlayerReadStatus::Eof;
    }
    return AvPlayerReadStatus::IoError;
}

inline bool AvPlayerReadBounceNeedsAlloc(u32 current_size, bool has_ptr, u32 needed) {
    return !has_ptr || current_size < needed;
}

inline u32 AvPlayerCopyFromCache(const u8* cache, u64 cache_size, u64 pos, u8* out, u32 len) {
    if (cache == nullptr || out == nullptr || pos >= cache_size || len == 0) {
        return 0;
    }
    const u64 avail = cache_size - pos;
    const u32 n = static_cast<u32>(std::min<u64>(len, avail));
    std::memcpy(out, cache + pos, n);
    return n;
}

} // namespace Libraries::AvPlayer
