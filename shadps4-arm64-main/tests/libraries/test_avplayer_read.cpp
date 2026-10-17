// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <gtest/gtest.h>

#include "core/libraries/avplayer/avplayer_read.h"

namespace {

using Libraries::AvPlayer::AvPlayerClassifyRead;
using Libraries::AvPlayer::AvPlayerCopyFromCache;
using Libraries::AvPlayer::AvPlayerGuestReadChunk;
using Libraries::AvPlayer::AvPlayerGuestReadChunkCount;
using Libraries::AvPlayer::AvPlayerReadBounceNeedsAlloc;
using Libraries::AvPlayer::AvPlayerReadStatus;

TEST(AvPlayerRead, MidFileZeroReadIsIoErrorNotEof) {
    // Driveclub attract: size()=53353705 but guest read returned 0 after ~0.6s.
    // Treating that as EOF freezes the first video frame for the rest of the movie.
    EXPECT_EQ(AvPlayerClassifyRead(0, 4096, 600000, 53353705), AvPlayerReadStatus::IoError);
}

TEST(AvPlayerRead, ZeroReadAtEndIsEof) {
    EXPECT_EQ(AvPlayerClassifyRead(0, 4096, 53353705, 53353705), AvPlayerReadStatus::Eof);
    EXPECT_EQ(AvPlayerClassifyRead(0, 0, 0, 53353705), AvPlayerReadStatus::Eof);
}

TEST(AvPlayerRead, PositiveReadIsOk) {
    EXPECT_EQ(AvPlayerClassifyRead(4096, 4096, 0, 53353705), AvPlayerReadStatus::Ok);
}

TEST(AvPlayerRead, BounceReusedWhenCapacityEnough) {
    EXPECT_FALSE(AvPlayerReadBounceNeedsAlloc(4096, true, 4096));
    EXPECT_FALSE(AvPlayerReadBounceNeedsAlloc(65536, true, 4096));
}

TEST(AvPlayerRead, BounceAllocatesWhenMissingOrTooSmall) {
    EXPECT_TRUE(AvPlayerReadBounceNeedsAlloc(0, false, 4096));
    EXPECT_TRUE(AvPlayerReadBounceNeedsAlloc(1024, true, 4096));
}

TEST(AvPlayerRead, GuestChunkNeverExceeds4k) {
    EXPECT_EQ(AvPlayerGuestReadChunk(65536), 4096u);
    EXPECT_EQ(AvPlayerGuestReadChunk(4096), 4096u);
    EXPECT_EQ(AvPlayerGuestReadChunk(100), 100u);
    EXPECT_EQ(AvPlayerGuestReadChunk(0), 0u);
}

TEST(AvPlayerRead, SixtyFourKAvioIsSixteenGuestChunks) {
    EXPECT_EQ(AvPlayerGuestReadChunkCount(65536), 16u);
    EXPECT_EQ(AvPlayerGuestReadChunkCount(4096), 1u);
    EXPECT_EQ(AvPlayerGuestReadChunkCount(4097), 2u);
    EXPECT_EQ(AvPlayerGuestReadChunkCount(0), 0u);
}

TEST(AvPlayerRead, CacheCopyServesHostIoAfterPreload) {
    const u8 cache[] = {1, 2, 3, 4, 5};
    u8 out[4]{};
    EXPECT_EQ(AvPlayerCopyFromCache(cache, 5, 0, out, 4), 4u);
    EXPECT_EQ(out[0], 1);
    EXPECT_EQ(out[3], 4);
    EXPECT_EQ(AvPlayerCopyFromCache(cache, 5, 4, out, 4), 1u);
    EXPECT_EQ(out[0], 5);
    EXPECT_EQ(AvPlayerCopyFromCache(cache, 5, 5, out, 4), 0u);
}

} // namespace
