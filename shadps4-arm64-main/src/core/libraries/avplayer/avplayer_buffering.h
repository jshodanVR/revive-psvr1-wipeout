// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "common/types.h"

namespace Libraries::AvPlayer {

// Playback-work predicate used by AvPlayerSource::HasFrames.
// `min_packets` is the packet watermark (0 while Playing, 10 while Buffering).
// Decoded frames always count as work so a hungry decoder that keeps the
// demux packet queue shallow cannot trap the player in StateBuffering.
inline bool AvPlayerHasPlaybackWork(u32 video_frames, u32 video_packets, bool eof,
                                    u32 min_packets) {
    return eof || video_frames > 0 || video_packets > min_packets;
}

inline bool AvPlayerShouldEnterBuffering(u32 video_frames, u32 video_packets, bool eof) {
    return !AvPlayerHasPlaybackWork(video_frames, video_packets, eof, 0);
}

inline bool AvPlayerShouldResumeFromBuffering(u32 video_frames, u32 video_packets, bool eof) {
    return AvPlayerHasPlaybackWork(video_frames, video_packets, eof, 10);
}

// Video/audio A/V gate used by AvPlayerSource::GetVideoData.
// Old behavior blocked whenever frame_ts > last_audio_ts, which freezes the
// last attract frame once audio ends (eof + empty audio queue).
inline bool AvPlayerVideoBlockedByAudio(u64 frame_ts, u64 last_audio_ts, bool have_audio,
                                        bool eof, u32 audio_frames) {
    if (!have_audio) {
        return false;
    }
    if (eof && audio_frames == 0) {
        return false;
    }
    return frame_ts > last_audio_ts;
}

} // namespace Libraries::AvPlayer
