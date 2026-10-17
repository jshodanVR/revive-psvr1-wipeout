// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "common/types.h"
#include "core/libraries/ngs2/ngs2.h"

// A native implementation of the parts of Ngs2 that titles drive directly: systems, racks,
// voices and the rendering of their audio. The exported sceNgs2* functions in ngs2.cpp are thin
// wrappers around these.

namespace Libraries::Ngs2::Engine {

s32 SystemCreate(const OrbisNgs2SystemOption* option, const OrbisNgs2ContextBufferInfo& buffer,
                 OrbisNgs2BufferFreeHandler free_handler, OrbisNgs2Handle* out_handle);
s32 SystemDestroy(OrbisNgs2Handle system, OrbisNgs2ContextBufferInfo* out_buffer);
s32 SystemRender(OrbisNgs2Handle system, const OrbisNgs2RenderBufferInfo* buffers, u32 count);
s32 SystemSetGrainSamples(OrbisNgs2Handle system, u32 samples);
s32 SystemSetSampleRate(OrbisNgs2Handle system, u32 sample_rate);
s32 SystemLock(OrbisNgs2Handle system);
s32 SystemUnlock(OrbisNgs2Handle system);
s32 SystemSetUserData(OrbisNgs2Handle system, uintptr_t user_data);
s32 SystemGetUserData(OrbisNgs2Handle system, uintptr_t* out_user_data);
s32 SystemGetInfo(OrbisNgs2Handle system, OrbisNgs2SystemInfo* out_info, size_t size);
s32 SystemEnumRackHandles(OrbisNgs2Handle system, OrbisNgs2Handle* out_handles, u32 max_handles);

s32 RackQueryBufferSize(u32 rack_id, const OrbisNgs2RackOption* option,
                        OrbisNgs2ContextBufferInfo* out_buffer);
s32 RackCreate(OrbisNgs2Handle system, u32 rack_id, const OrbisNgs2RackOption* option,
               const OrbisNgs2ContextBufferInfo& buffer, OrbisNgs2BufferFreeHandler free_handler,
               OrbisNgs2Handle* out_handle);
s32 RackDestroy(OrbisNgs2Handle rack, OrbisNgs2ContextBufferInfo* out_buffer);
s32 RackGetVoiceHandle(OrbisNgs2Handle rack, u32 voice_index, OrbisNgs2Handle* out_handle);
s32 RackGetInfo(OrbisNgs2Handle rack, OrbisNgs2RackInfo* out_info, size_t size);
s32 RackSetUserData(OrbisNgs2Handle rack, uintptr_t user_data);
s32 RackGetUserData(OrbisNgs2Handle rack, uintptr_t* out_user_data);

s32 VoiceControl(OrbisNgs2Handle voice, const OrbisNgs2VoiceParamHeader* params);
s32 VoiceGetState(OrbisNgs2Handle voice, OrbisNgs2VoiceState* out_state, size_t size);
s32 VoiceGetStateFlags(OrbisNgs2Handle voice, u32* out_flags);
s32 VoiceGetOwner(OrbisNgs2Handle voice, OrbisNgs2Handle* out_rack, u32* out_index);
s32 VoiceGetPortInfo(OrbisNgs2Handle voice, u32 port, OrbisNgs2VoicePortInfo* out_info,
                     size_t size);
s32 VoiceGetMatrixInfo(OrbisNgs2Handle voice, u32 matrix, OrbisNgs2VoiceMatrixInfo* out_info,
                       size_t size);

/// Calls a title-provided buffer allocation or release handler.
s32 CallBufferHandler(OrbisNgs2BufferAllocHandler handler, OrbisNgs2ContextBufferInfo* info);

} // namespace Libraries::Ngs2::Engine
