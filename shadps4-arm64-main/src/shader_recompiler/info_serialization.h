// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "common/serdes.h"
#include "shader_recompiler/info.h"

namespace Shader {

// Image/sampler lists are small_vectors: even their inline storage has a host
// pointer. Raw-copying InfoPersistent to disk resurrects pointers into a dead
// process during pipeline warmup. Serialize elements and scalar fields only.
template <typename InfoType, typename Visit>
void VisitPersistentInfo(InfoType& info, Visit&& visit) {
    visit(info.buffers);
    visit(info.images);
    visit(info.samplers);
    visit(info.fmasks);
    visit(info.ud_mask);
    visit(info.fetch_shader_sgpr_base);
    visit(info.pgm_hash);
    visit(info.tess_consts_dword_offset);
    visit(info.tess_consts_ptr_base);
    visit(info.stage);
    visit(info.l_stage);
    visit(info.mrt_mask);
    visit(info.has_fetch_shader);
    visit(info.has_bitwise_xor);
    visit(info.uses_dma);
}

inline void WritePersistentInfo(Serialization::Writer& writer, const InfoPersistent& info) {
    VisitPersistentInfo(info, [&](const auto& field) { writer.Write(field); });
}

inline void ReadPersistentInfo(Serialization::Reader& reader, InfoPersistent& info) {
    info.buffers.clear();
    info.images.clear();
    info.samplers.clear();
    info.fmasks.clear();
    VisitPersistentInfo(info, [&](auto& field) { reader.Read(field); });
}

} // namespace Shader
