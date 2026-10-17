// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <gtest/gtest.h>

#include "core/libraries/avplayer/avplayer_buffering.h"
#include "core/libraries/avplayer/avplayer_path.h"

namespace {

using Libraries::AvPlayer::AvPlayerNormalizeGuestPath;
using Libraries::AvPlayer::AvPlayerVideoBlockedByAudio;

TEST(AvPlayerPath, DriveclubBackslashAttractPathBecomesPosix) {
    EXPECT_EQ(AvPlayerNormalizeGuestPath("newui\\art\\dc_attract_video_30fps_11.v3_256.mp4"),
              "newui/art/dc_attract_video_30fps_11.v3_256.mp4");
}

TEST(AvPlayerPath, ForwardSlashesUnchanged) {
    EXPECT_EQ(AvPlayerNormalizeGuestPath("/app0/newui/art/foo.mp4"), "/app0/newui/art/foo.mp4");
}

TEST(AvPlayerSync, VideoWaitsWhileAudioIsStillAlive) {
    EXPECT_TRUE(AvPlayerVideoBlockedByAudio(333, 320, true, false, 0));
}

TEST(AvPlayerSync, EofWithEmptyAudioQueueDoesNotFreezeVideo) {
    // Driveclub attract: demux_eof then last_audio_ts stuck, remaining video frames
    // have a later timestamp and would otherwise never leave GetVideoData.
    EXPECT_FALSE(AvPlayerVideoBlockedByAudio(333, 320, true, true, 0));
}

TEST(AvPlayerSync, EofStillSyncsWhileAudioFramesRemain) {
    EXPECT_TRUE(AvPlayerVideoBlockedByAudio(400, 320, true, true, 2));
}

TEST(AvPlayerSync, NoAudioStreamNeverBlocks) {
    EXPECT_FALSE(AvPlayerVideoBlockedByAudio(100, 0, false, false, 0));
}

} // namespace
