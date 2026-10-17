// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <gtest/gtest.h>

#include "core/libraries/avplayer/avplayer_buffering.h"

namespace {

using Libraries::AvPlayer::AvPlayerHasPlaybackWork;
using Libraries::AvPlayer::AvPlayerShouldEnterBuffering;
using Libraries::AvPlayer::AvPlayerShouldResumeFromBuffering;

TEST(AvPlayerBuffering, EmptyQueuesEnterBuffering) {
    EXPECT_TRUE(AvPlayerShouldEnterBuffering(0, 0, false));
    EXPECT_FALSE(AvPlayerHasPlaybackWork(0, 0, false, 0));
}

TEST(AvPlayerBuffering, EofNeverEntersBuffering) {
    EXPECT_FALSE(AvPlayerShouldEnterBuffering(0, 0, true));
    EXPECT_TRUE(AvPlayerHasPlaybackWork(0, 0, true, 10));
}

TEST(AvPlayerBuffering, ShallowPacketQueueKeepsPlayWhenAnyPacketExists) {
    EXPECT_FALSE(AvPlayerShouldEnterBuffering(0, 5, false));
    EXPECT_TRUE(AvPlayerHasPlaybackWork(0, 5, false, 0));
}

TEST(AvPlayerBuffering, ShallowPacketQueueAloneDoesNotResume) {
    EXPECT_FALSE(AvPlayerShouldResumeFromBuffering(0, 5, false));
    EXPECT_FALSE(AvPlayerHasPlaybackWork(0, 5, false, 10));
}

TEST(AvPlayerBuffering, PacketWatermarkStillResumes) {
    EXPECT_TRUE(AvPlayerShouldResumeFromBuffering(0, 11, false));
    EXPECT_TRUE(AvPlayerHasPlaybackWork(0, 11, false, 10));
}

TEST(AvPlayerBuffering, DecodedFramesResumeEvenWhenPacketsAreEmpty) {
    // The Driveclub stall: 16 decoded frames, 0 demux packets, resume min=10.
    EXPECT_TRUE(AvPlayerShouldResumeFromBuffering(16, 0, false));
    EXPECT_TRUE(AvPlayerHasPlaybackWork(16, 0, false, 10));
    EXPECT_FALSE(AvPlayerShouldEnterBuffering(16, 0, false));
}

TEST(AvPlayerBuffering, SingleDecodedFrameIsEnoughToStayInPlayOrResume) {
    EXPECT_TRUE(AvPlayerShouldResumeFromBuffering(1, 0, false));
    EXPECT_FALSE(AvPlayerShouldEnterBuffering(1, 3, false));
}

} // namespace
