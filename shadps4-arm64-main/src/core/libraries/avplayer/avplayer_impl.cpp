// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <cstring>

#include "common/logging/log.h"
#include "core/libraries/avplayer/avplayer_common.h"
#include "core/libraries/avplayer/avplayer_error.h"
#include "core/libraries/avplayer/avplayer_impl.h"
#include "core/libraries/avplayer/avplayer_path.h"
#include "core/libraries/avplayer/avplayer_read.h"
#ifdef SHADPS4_ENABLE_FEX_GUEST_CPU
#include "core/guest_cpu/guest_callback.h"
#endif

namespace Libraries::AvPlayer {

#ifdef SHADPS4_ENABLE_FEX_GUEST_CPU
template <typename Callback>
const void* CallbackAddress(Callback callback) {
    return reinterpret_cast<const void*>(callback);
}

template <typename Callback>
bool IsGuestCallback(Callback callback) {
    return Core::GuestCpu::IsGuestFunctionAddress(CallbackAddress(callback));
}
#endif

void* PS4_SYSV_ABI AvPlayer::Allocate(void* handle, u32 alignment, u32 size) {
    const auto* const self = reinterpret_cast<AvPlayer*>(handle);
    const auto allocate = self->m_init_data_original.memory_replacement.allocate;
    const auto ptr = self->m_init_data_original.memory_replacement.object_ptr;
#ifdef SHADPS4_ENABLE_FEX_GUEST_CPU
    if (IsGuestCallback(allocate)) {
        return reinterpret_cast<void*>(Core::GuestCpu::RunGuestFunctionOrAbort(
            CallbackAddress(allocate), "AvPlayer allocate", ptr, alignment, size));
    }
#endif
    return allocate(ptr, alignment, size);
}

void PS4_SYSV_ABI AvPlayer::Deallocate(void* handle, void* memory) {
    const auto* const self = reinterpret_cast<AvPlayer*>(handle);
    const auto deallocate = self->m_init_data_original.memory_replacement.deallocate;
    const auto ptr = self->m_init_data_original.memory_replacement.object_ptr;
#ifdef SHADPS4_ENABLE_FEX_GUEST_CPU
    if (IsGuestCallback(deallocate)) {
        Core::GuestCpu::RunGuestFunctionOrAbort(CallbackAddress(deallocate),
                                                "AvPlayer deallocate", ptr, memory);
        return;
    }
#endif
    deallocate(ptr, memory);
}

void* PS4_SYSV_ABI AvPlayer::AllocateTexture(void* handle, u32 alignment, u32 size) {
    const auto* const self = reinterpret_cast<AvPlayer*>(handle);
    const auto allocate = self->m_init_data_original.memory_replacement.allocate_texture;
    const auto ptr = self->m_init_data_original.memory_replacement.object_ptr;
#ifdef SHADPS4_ENABLE_FEX_GUEST_CPU
    if (IsGuestCallback(allocate)) {
        return reinterpret_cast<void*>(Core::GuestCpu::RunGuestFunctionOrAbort(
            CallbackAddress(allocate), "AvPlayer allocate texture", ptr, alignment, size));
    }
#endif
    return allocate(ptr, alignment, size);
}

void PS4_SYSV_ABI AvPlayer::DeallocateTexture(void* handle, void* memory) {
    const auto* const self = reinterpret_cast<AvPlayer*>(handle);
    const auto deallocate = self->m_init_data_original.memory_replacement.deallocate_texture;
    const auto ptr = self->m_init_data_original.memory_replacement.object_ptr;
#ifdef SHADPS4_ENABLE_FEX_GUEST_CPU
    if (IsGuestCallback(deallocate)) {
        Core::GuestCpu::RunGuestFunctionOrAbort(
            CallbackAddress(deallocate), "AvPlayer deallocate texture", ptr, memory);
        return;
    }
#endif
    deallocate(ptr, memory);
}

int PS4_SYSV_ABI AvPlayer::OpenFile(void* handle, const char* filename) {
    auto const self = reinterpret_cast<AvPlayer*>(handle);
    std::lock_guard guard(self->m_file_io_mutex);

    const auto open = self->m_init_data_original.file_replacement.open;
    const auto ptr = self->m_init_data_original.file_replacement.object_ptr;
#ifdef SHADPS4_ENABLE_FEX_GUEST_CPU
    if (IsGuestCallback(open)) {
        const auto normalized = AvPlayerNormalizeGuestPath(filename ? filename : "");
        const auto name_len = static_cast<u32>(normalized.size() + 1);
        void* guest_name = Allocate(handle, 1, name_len);
        if (guest_name == nullptr) {
            return -1;
        }
        std::memcpy(guest_name, normalized.c_str(), name_len);
        const auto fd = static_cast<int>(Core::GuestCpu::RunGuestFunctionOrAbort(
            CallbackAddress(open), "AvPlayer open", ptr, guest_name));
        Deallocate(handle, guest_name);
        return fd;
    }
#endif
    const auto normalized = AvPlayerNormalizeGuestPath(filename ? filename : "");
    return open(ptr, normalized.c_str());
}

int PS4_SYSV_ABI AvPlayer::CloseFile(void* handle) {
    auto const self = reinterpret_cast<AvPlayer*>(handle);
    std::lock_guard guard(self->m_file_io_mutex);

    const auto close = self->m_init_data_original.file_replacement.close;
    const auto ptr = self->m_init_data_original.file_replacement.object_ptr;
#ifdef SHADPS4_ENABLE_FEX_GUEST_CPU
    if (IsGuestCallback(close)) {
        return static_cast<int>(Core::GuestCpu::RunGuestFunctionOrAbort(
            CallbackAddress(close), "AvPlayer close", ptr));
    }
#endif
    return close(ptr);
}

bool AvPlayer::EnsureFileCacheLocked() {
#ifdef SHADPS4_ENABLE_FEX_GUEST_CPU
    if (m_file_cache_ready) {
        return true;
    }
    const auto read_offset = m_init_data_original.file_replacement.read_offset;
    const auto size_fn = m_init_data_original.file_replacement.size;
    const auto ptr = m_init_data_original.file_replacement.object_ptr;
    if (!IsGuestCallback(read_offset)) {
        m_file_cache_ready = true;
        return true;
    }
    u64 expected = 0;
    if (size_fn != nullptr) {
        if (IsGuestCallback(size_fn)) {
            expected = Core::GuestCpu::RunGuestFunctionOrAbort(CallbackAddress(size_fn),
                                                               "AvPlayer size", ptr);
        } else {
            expected = size_fn(ptr);
        }
    }
    if (m_file_read_bounce == nullptr) {
        m_file_read_bounce = Allocate(this, 16, kAvPlayerGuestReadChunkMax);
        if (m_file_read_bounce == nullptr) {
            LOG_ERROR(Lib_AvPlayer, "AvPlayer preload bounce allocate failed");
            return false;
        }
        m_file_read_bounce_size = kAvPlayerGuestReadChunkMax;
    }
    if (expected > 0 && expected < 256 * 1024 * 1024) {
        m_file_cache.reserve(static_cast<size_t>(expected));
    }
    u64 pos = 0;
    while (expected == 0 || pos < expected) {
        const auto bytes_read = static_cast<int>(Core::GuestCpu::RunGuestFunctionOrAbort(
            CallbackAddress(read_offset), "AvPlayer read_offset", ptr, m_file_read_bounce, pos,
            kAvPlayerGuestReadChunkMax));
        if (bytes_read <= 0) {
            break;
        }
        const auto got = std::min(static_cast<u32>(bytes_read), kAvPlayerGuestReadChunkMax);
        const auto* src = static_cast<const u8*>(m_file_read_bounce);
        m_file_cache.insert(m_file_cache.end(), src, src + got);
        pos += got;
        if (got < kAvPlayerGuestReadChunkMax) {
            break;
        }
    }
    m_file_cache_ready = true;
    LOG_INFO(Lib_AvPlayer, "AvPlayer preloaded {} bytes (size_cb={})", m_file_cache.size(),
             expected);
    return true;
#else
    m_file_cache_ready = true;
    return true;
#endif
}

int PS4_SYSV_ABI AvPlayer::ReadOffsetFile(void* handle, u8* buffer, u64 position, u32 length) {
    auto const self = reinterpret_cast<AvPlayer*>(handle);
    std::lock_guard guard(self->m_file_io_mutex);

    const auto read_offset = self->m_init_data_original.file_replacement.read_offset;
    const auto ptr = self->m_init_data_original.file_replacement.object_ptr;
#ifdef SHADPS4_ENABLE_FEX_GUEST_CPU
    if (IsGuestCallback(read_offset)) {
        if (!self->EnsureFileCacheLocked()) {
            return 0;
        }
        return static_cast<int>(
            AvPlayerCopyFromCache(self->m_file_cache.data(), self->m_file_cache.size(), position,
                                  buffer, length));
    }
#endif
    return read_offset(ptr, buffer, position, length);
}

u64 PS4_SYSV_ABI AvPlayer::SizeFile(void* handle) {
    auto const self = reinterpret_cast<AvPlayer*>(handle);
    std::lock_guard guard(self->m_file_io_mutex);

    const auto size = self->m_init_data_original.file_replacement.size;
    const auto ptr = self->m_init_data_original.file_replacement.object_ptr;
#ifdef SHADPS4_ENABLE_FEX_GUEST_CPU
    if (IsGuestCallback(size)) {
        if (self->EnsureFileCacheLocked()) {
            return self->m_file_cache.size();
        }
        return Core::GuestCpu::RunGuestFunctionOrAbort(CallbackAddress(size), "AvPlayer size",
                                                       ptr);
    }
#endif
    return size(ptr);
}

AvPlayerInitData AvPlayer::StubInitData(const AvPlayerInitData& data) {
    AvPlayerInitData result = data;
    result.memory_replacement.object_ptr = this;
    result.memory_replacement.allocate = &AvPlayer::Allocate;
    result.memory_replacement.deallocate = &AvPlayer::Deallocate;
    result.memory_replacement.allocate_texture = &AvPlayer::AllocateTexture;
    result.memory_replacement.deallocate_texture = &AvPlayer::DeallocateTexture;
    const bool missing_file_callback =
        data.file_replacement.open == nullptr || data.file_replacement.close == nullptr ||
        data.file_replacement.read_offset == nullptr || data.file_replacement.size == nullptr;
    if (missing_file_callback) {
        result.file_replacement = {};
    } else {
        result.file_replacement.object_ptr = this;
        result.file_replacement.open = &AvPlayer::OpenFile;
        result.file_replacement.close = &AvPlayer::CloseFile;
        result.file_replacement.read_offset = &AvPlayer::ReadOffsetFile;
        result.file_replacement.size = &AvPlayer::SizeFile;
    }
    return result;
}

AvPlayer::AvPlayer(const AvPlayerInitData& data)
    : m_init_data(StubInitData(data)), m_init_data_original(data),
      m_state(std::make_unique<AvPlayerState>(m_init_data)) {}

AvPlayer::~AvPlayer() {
    if (m_file_read_bounce != nullptr) {
        Deallocate(this, m_file_read_bounce);
        m_file_read_bounce = nullptr;
        m_file_read_bounce_size = 0;
    }
    m_file_cache.clear();
    m_file_cache_ready = false;
}

s32 AvPlayer::PostInit(const AvPlayerPostInitData& data) {
    m_state->PostInit(data);
    return ORBIS_OK;
}

s32 AvPlayer::AddSource(std::string_view path) {
    return AddSourceEx(path, AvPlayerSourceType::Unknown);
}

s32 AvPlayer::AddSourceEx(std::string_view path, AvPlayerSourceType source_type) {
    if (source_type == AvPlayerSourceType::Unknown) {
        source_type = GetSourceType(path);
    }
    if (source_type == AvPlayerSourceType::Hls) {
        LOG_ERROR(Lib_AvPlayer, "HTTP Live Streaming is not implemented");
        return ORBIS_AVPLAYER_ERROR_NOT_SUPPORTED;
    }
    if (!m_state->AddSource(path, GetSourceType(path))) {
        return ORBIS_AVPLAYER_ERROR_OPERATION_FAILED;
    }
    return ORBIS_OK;
}

s32 AvPlayer::GetStreamCount() {
    if (m_state == nullptr) {
        return ORBIS_AVPLAYER_ERROR_OPERATION_FAILED;
    }
    const auto res = m_state->GetStreamCount();
    if (AVPLAYER_IS_ERROR(res)) {
        return ORBIS_AVPLAYER_ERROR_OPERATION_FAILED;
    }
    return res;
}

s32 AvPlayer::GetStreamInfo(u32 stream_index, AvPlayerStreamInfo& info) {
    if (!m_state->GetStreamInfo(stream_index, info)) {
        return ORBIS_AVPLAYER_ERROR_OPERATION_FAILED;
    }
    return ORBIS_OK;
}

s32 AvPlayer::EnableStream(u32 stream_index) {
    if (m_state == nullptr) {
        return ORBIS_AVPLAYER_ERROR_OPERATION_FAILED;
    }
    if (!m_state->EnableStream(stream_index)) {
        return ORBIS_AVPLAYER_ERROR_OPERATION_FAILED;
    }
    return ORBIS_OK;
}

s32 AvPlayer::Start() {
    if (m_state == nullptr || !m_state->Start()) {
        return ORBIS_AVPLAYER_ERROR_OPERATION_FAILED;
    }
    return ORBIS_OK;
}

s32 AvPlayer::Pause() {
    if (m_state == nullptr || !m_state->Pause()) {
        return ORBIS_AVPLAYER_ERROR_OPERATION_FAILED;
    }
    return ORBIS_OK;
}

s32 AvPlayer::Resume() {
    if (m_state == nullptr || !m_state->Resume()) {
        return ORBIS_AVPLAYER_ERROR_OPERATION_FAILED;
    }
    return ORBIS_OK;
}

s32 AvPlayer::SetAvSyncMode(AvPlayerAvSyncMode sync_mode) {
    if (m_state == nullptr) {
        return ORBIS_AVPLAYER_ERROR_OPERATION_FAILED;
    }
    m_state->SetAvSyncMode(sync_mode);
    return ORBIS_OK;
}

bool AvPlayer::GetVideoData(AvPlayerFrameInfo& video_info) {
    if (m_state == nullptr) {
        return false;
    }
    return m_state->GetVideoData(video_info);
}

bool AvPlayer::GetVideoData(AvPlayerFrameInfoEx& video_info) {
    if (m_state == nullptr) {
        return false;
    }
    return m_state->GetVideoData(video_info);
}

bool AvPlayer::GetAudioData(AvPlayerFrameInfo& audio_info) {
    if (m_state == nullptr) {
        return false;
    }
    return m_state->GetAudioData(audio_info);
}

bool AvPlayer::IsActive() {
    if (m_state == nullptr) {
        return false;
    }
    return m_state->IsActive();
}

u64 AvPlayer::CurrentTime() {
    if (m_state == nullptr) {
        return 0;
    }
    return m_state->CurrentTime();
}

s32 AvPlayer::Stop() {
    if (m_state == nullptr || !m_state->Stop()) {
        return ORBIS_AVPLAYER_ERROR_OPERATION_FAILED;
    }
    return ORBIS_OK;
}

bool AvPlayer::SetLooping(bool is_looping) {
    if (m_state == nullptr) {
        return false;
    }
    return m_state->SetLooping(is_looping);
}

} // namespace Libraries::AvPlayer
