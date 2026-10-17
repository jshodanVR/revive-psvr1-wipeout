// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "common/debug.h"
#include "common/elf_info.h"
#include "core/emulator_settings.h"
#include "core/memory.h"
#include "core/known_title_rush_resolution_trial.h"
#include "shader_recompiler/runtime_info.h"
#include "video_core/amdgpu/liverpool.h"
#include "video_core/amdgpu/htile_stencil.h"
#include "common/alignment.h"
#include "video_core/renderer_vulkan/liverpool_to_vk.h"
#include "video_core/renderer_vulkan/readconst_snapshot_diag.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_rasterizer.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_shader_hle.h"

#include <array>
#include <bit>
#include <fstream>
#include "common/path_util.h"
#ifdef _WIN32
#include <windows.h>
#endif

#include <vk_mem_alloc.h>
#include "video_core/texture_cache/image_view.h"
#include "video_core/texture_cache/texture_cache.h"

#ifdef MemoryBarrier
#undef MemoryBarrier
#endif

namespace Vulkan {

static Shader::PushData MakeUserData(const AmdGpu::Regs& regs) {
    // TODO(roamic): Add support for multiple viewports and geometry shaders when ViewportIndex
    // is encountered and implemented in the recompiler.
    Shader::PushData push_data{};
    push_data.xoffset = regs.viewport_control.xoffset_enable ? regs.viewports[0].xoffset : 0.f;
    push_data.xscale = regs.viewport_control.xscale_enable ? regs.viewports[0].xscale : 1.f;
    push_data.yoffset = regs.viewport_control.yoffset_enable ? regs.viewports[0].yoffset : 0.f;
    push_data.yscale = regs.viewport_control.yscale_enable ? regs.viewports[0].yscale : 1.f;
    return push_data;
}

Rasterizer::Rasterizer(const Instance& instance_, Scheduler& scheduler_,
                       AmdGpu::Liverpool* liverpool_)
    : instance{instance_}, scheduler{scheduler_}, page_manager{this},
      buffer_cache{instance, scheduler, liverpool_, texture_cache, page_manager},
      texture_cache{instance, scheduler, liverpool_, buffer_cache, page_manager},
      liverpool{liverpool_}, memory{Core::Memory::Instance()},
      pipeline_cache{instance, scheduler, liverpool} {
    if (!EmulatorSettings.IsNullGPU()) {
        liverpool->BindRasterizer(this);
    }
    memory->SetRasterizer(this);
}

Rasterizer::~Rasterizer() = default;

static std::atomic<s32> g_draw_trace_budget{0};
static std::atomic<s32> g_buffer_write_trace_budget{[] {
    const char* value = std::getenv("SHADPS4_TRACE_BUFFER_WRITES_ON_START");
    return value && std::string_view{value} == "1" ? 200000 : 0;
}()};

void Rasterizer::StartBufferWriteTrace(s32 count) {
    g_buffer_write_trace_budget.store(count);
    LOG_INFO(Render_Vulkan, "BUFFER_WRITES armed budget={}", count);
}

bool Rasterizer::TraceBufferWrite(VAddr address, u64 size) {
    if (g_buffer_write_trace_budget.load(std::memory_order_relaxed) <= 0) return false;
    static const auto range = [] {
        std::pair<u64, u64> result{};
        if (const char* value = std::getenv("SHADPS4_TRACE_BUFFER_WRITE_RANGE")) {
            char* end = nullptr;
            result.first = std::strtoull(value, &end, 16);
            if (end && *end == ':') result.second = std::strtoull(end + 1, nullptr, 16);
        }
        return result;
    }();
    if (range.second <= range.first || !size) return false;
    // Address zero denotes a special shader resource whose range is not statically known.
    if (address && (address >= range.second ||
        (address < range.first && size <= range.first - address))) return false;
    return g_buffer_write_trace_budget.fetch_sub(1) > 0;
}

void Rasterizer::StartDrawTrace(s32 count) {
    g_draw_trace_budget.store(count);
}

static bool TraceDraw() {
    if (g_draw_trace_budget.load(std::memory_order_relaxed) <= 0) {
        return false;
    }
    return g_draw_trace_budget.fetch_sub(1) > 0;
}

void Rasterizer::CpSync() {
    scheduler.EndRendering();
    auto cmdbuf = scheduler.CommandBuffer();

    const vk::MemoryBarrier ib_barrier{
        .srcAccessMask = vk::AccessFlagBits::eShaderWrite,
        .dstAccessMask = vk::AccessFlagBits::eIndirectCommandRead,
    };
    cmdbuf.pipelineBarrier(vk::PipelineStageFlagBits::eComputeShader,
                           vk::PipelineStageFlagBits::eDrawIndirect,
                           vk::DependencyFlagBits::eByRegion, ib_barrier, {}, {});
}

bool Rasterizer::FilterDraw() {
    const auto& regs = liverpool->regs;
    if (regs.color_control.mode == AmdGpu::ColorControl::OperationMode::EliminateFastClear) {
        // Clears the render target if FCE is launched before any draws
        if (TraceDraw()) {
            const auto& col_buf = regs.color_buffers[0];
            LOG_INFO(Render_Vulkan, "TRACE fce cb0={:#x} fast_clear={} meta_cleared={}",
                     col_buf.Address(), static_cast<bool>(col_buf.info.fast_clear),
                     texture_cache.IsMetaCleared(col_buf.CmaskAddress(), col_buf.view.slice_start));
        }
        EliminateFastClear();
        return false;
    }
    if (regs.color_control.mode == AmdGpu::ColorControl::OperationMode::FmaskDecompress) {
        // TODO: check for a valid MRT1 to promote the draw to the resolve pass.
        LOG_TRACE(Render_Vulkan, "FMask decompression pass skipped");
        if (TraceDraw()) {
            LOG_INFO(Render_Vulkan, "TRACE fmask-decompress cb0={:#x}",
                     regs.color_buffers[0].Address());
        }
        ScopedMarkerInsert("FmaskDecompress");
        return false;
    }
    if (regs.color_control.mode == AmdGpu::ColorControl::OperationMode::Resolve) {
        LOG_TRACE(Render_Vulkan, "Resolve pass");
        if (TraceDraw()) {
            LOG_INFO(Render_Vulkan, "TRACE resolve cb0={:#x} -> cb1={:#x}",
                     regs.color_buffers[0].Address(), regs.color_buffers[1].Address());
        }
        Resolve();
        return false;
    }
    if (regs.primitive_type == AmdGpu::PrimitiveType::None) {
        LOG_TRACE(Render_Vulkan, "Primitive type 'None' skipped");
        ScopedMarkerInsert("PrimitiveTypeNone");
        return false;
    }

    const bool cb_disabled =
        regs.color_control.mode == AmdGpu::ColorControl::OperationMode::Disable;
    const auto depth_copy =
        regs.depth_render_override.force_z_dirty && regs.depth_render_override.force_z_valid &&
        regs.depth_buffer.DepthValid() && regs.depth_buffer.DepthWriteValid() &&
        regs.depth_buffer.DepthAddress() != regs.depth_buffer.DepthWriteAddress();
    const auto stencil_copy =
        regs.depth_render_override.force_stencil_dirty &&
        regs.depth_render_override.force_stencil_valid && regs.depth_buffer.StencilValid() &&
        regs.depth_buffer.StencilWriteValid() &&
        regs.depth_buffer.StencilAddress() != regs.depth_buffer.StencilWriteAddress();
    if (cb_disabled && (depth_copy || stencil_copy)) {
        if (TraceDraw()) {
            LOG_INFO(Render_Vulkan,
                     "TRACE depth-stencil-copy depth={} stencil={} src={:#x}/{:#x} dst={:#x}/{:#x}",
                     depth_copy, stencil_copy, regs.depth_buffer.DepthAddress(),
                     regs.depth_buffer.StencilAddress(), regs.depth_buffer.DepthWriteAddress(),
                     regs.depth_buffer.StencilWriteAddress());
        }
        // Games may disable color buffer and enable force depth/stencil dirty and valid to
        // do a copy from one depth-stencil surface to another, without a pixel shader.
        // We need to detect this case and perform the copy, otherwise it will have no effect.
        LOG_TRACE(Render_Vulkan, "Performing depth-stencil override copy");
        DepthStencilCopy(depth_copy, stencil_copy);
        return false;
    }

    return true;
}

namespace {

struct DepthTargetState {
    vk::ImageLayout layout;
    vk::AccessFlags2 access;
};

// Statik clears stencil with screen-space rectangles between its eye passes.
// A render-pass loadOp clear ignores that geometry and erases the other eye's
// decal receiver bits. Execute the actual clear rectangle as a stencil draw.
bool StatikStencilClearDraw(const AmdGpu::Regs& regs) {
    static const bool enabled = [] {
        const char* trial = std::getenv("SHADPS4_TEST_STATIK_STENCIL_CLEAR");
        return (!trial || std::string_view{trial} != "0") &&
               Common::ElfInfo::Instance().GameSerial() == "CUSA06929" &&
               Common::ElfInfo::Instance().AppVer() == "01.00";
    }();
    return enabled && regs.primitive_type == AmdGpu::PrimitiveType::RectList &&
           regs.depth_render_control.stencil_clear_enable &&
           !regs.depth_render_control.depth_clear_enable &&
           !regs.depth_control.depth_enable && regs.depth_control.stencil_enable &&
           regs.depth_buffer.StencilValid() && regs.color_target_mask.raw == 0;
}

/// The layout a depth buffer is attached in for a draw, and what the draw does with it there.
///
/// The layouts that allow writing serve draws with and without depth writes alike. Titles
/// switch depth writes on and off from one draw to the next (a decal, then an opaque object,
/// then a particle): giving the two kinds different layouts ended the render pass at every
/// switch, a hundred times a frame. Sampling requires a read-only layout when there are
/// no depth writes, or a feedback layout when the same draw also writes depth.
DepthTargetState GetDepthTargetState(const VideoCore::Image& image, const AmdGpu::Regs& regs,
                                    bool feedback_layout_supported) {
    const bool has_stencil = image.info.props.has_stencil;
    const bool writes_depth = regs.depth_control.depth_write_enable;
    const bool sampled = image.binding.is_bound;
    if (sampled && writes_depth) {
        return {feedback_layout_supported ? vk::ImageLayout::eAttachmentFeedbackLoopOptimalEXT
                                          : vk::ImageLayout::eGeneral,
                vk::AccessFlagBits2::eShaderRead |
                    vk::AccessFlagBits2::eDepthStencilAttachmentWrite |
                    vk::AccessFlagBits2::eDepthStencilAttachmentRead};
    }
    if (writes_depth || !sampled) {
        return {has_stencil ? vk::ImageLayout::eDepthStencilAttachmentOptimal
                            : vk::ImageLayout::eDepthAttachmentOptimal,
                vk::AccessFlagBits2::eDepthStencilAttachmentWrite |
                    vk::AccessFlagBits2::eDepthStencilAttachmentRead};
    }
    // Stencil writes can be enabled while depth writes are off.
    if (has_stencil && regs.depth_control.stencil_enable) {
        return {vk::ImageLayout::eDepthReadOnlyStencilAttachmentOptimal,
                vk::AccessFlagBits2::eShaderRead |
                    vk::AccessFlagBits2::eDepthStencilAttachmentRead |
                    vk::AccessFlagBits2::eDepthStencilAttachmentWrite};
    }
    return {has_stencil ? vk::ImageLayout::eDepthStencilReadOnlyOptimal
                        : vk::ImageLayout::eDepthReadOnlyOptimal,
            // The render pass still uses STORE even when fragment depth writes are
            // disabled. Include its write so the next layout transition waits for it.
            vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eDepthStencilAttachmentRead |
                vk::AccessFlagBits2::eDepthStencilAttachmentWrite};
}

} // namespace

void Rasterizer::PrepareRenderState(const GraphicsPipeline* pipeline) {
    // Prefetch render targets to handle overlaps with bound textures (e.g. mipgen)
    const auto& key = pipeline->GetGraphicsKey();
    const auto& regs = liverpool->regs;
    if (regs.color_control.degamma_enable) {
        LOG_WARNING(Render_Vulkan, "Color buffers require gamma correction");
    }

    const bool skip_cb_binding =
        regs.color_control.mode == AmdGpu::ColorControl::OperationMode::Disable;
    for (s32 cb = 0; cb < std::bit_width(key.mrt_mask); ++cb) {
        auto& [image_id, desc] = cb_descs[cb];
        const auto& col_buf = regs.color_buffers[cb];
        const u32 target_mask = regs.color_target_mask.GetMask(cb);
        if (skip_cb_binding || !col_buf || !target_mask || (key.mrt_mask & (1 << cb)) == 0) {
            image_id = {};
            continue;
        }
        const auto& hint = liverpool->last_cb_extent[cb];
        std::construct_at(&desc, col_buf, hint);
        image_id = bound_images.emplace_back(texture_cache.FindImage(desc));
        auto& image = texture_cache.GetImage(image_id);
        image.binding.is_target = 1u;
    }

    if ((regs.depth_control.depth_enable && regs.depth_buffer.DepthValid()) ||
        (regs.depth_control.stencil_enable && regs.depth_buffer.StencilValid())) {
        const auto htile_address = regs.depth_htile_data_base.GetAddress();
        const auto& hint = liverpool->last_db_extent;
        auto& [image_id, desc] = db_desc;
        std::construct_at(&desc, regs.depth_buffer, regs.depth_view, regs.depth_control,
                          htile_address, hint);
        image_id = bound_images.emplace_back(texture_cache.FindImage(desc));
        auto& image = texture_cache.GetImage(image_id);
        image.binding.is_target = 1u;
    } else {
        db_desc.first = {};
    }
}

static std::pair<u32, u32> GetDrawOffsets(
    const AmdGpu::Regs& regs, const Shader::Info& info,
    const std::optional<Shader::Gcn::FetchShaderData>& fetch_shader) {
    u32 vertex_offset = regs.index_offset;
    u32 instance_offset = 0;
    if (fetch_shader) {
        if (vertex_offset == 0 && fetch_shader->vertex_offset_sgpr != -1) {
            vertex_offset = info.user_data[fetch_shader->vertex_offset_sgpr];
        }
        if (fetch_shader->instance_offset_sgpr != -1) {
            instance_offset = info.user_data[fetch_shader->instance_offset_sgpr];
        }
    }
    return {vertex_offset, instance_offset};
}

void Rasterizer::EliminateFastClear() {
    auto& col_buf = liverpool->regs.color_buffers[0];
    if (!col_buf || !col_buf.info.fast_clear) {
        return;
    }
    VideoCore::TextureCache::ImageDesc desc(col_buf, liverpool->last_cb_extent[0]);
    const auto image_id = texture_cache.FindImage(desc);
    const auto& image_view = texture_cache.FindRenderTarget(image_id, desc);
    if (!texture_cache.IsMetaCleared(col_buf.CmaskAddress(), col_buf.view.slice_start)) {
        return;
    }
    for (u32 slice = col_buf.view.slice_start; slice <= col_buf.view.slice_max; ++slice) {
        texture_cache.TouchMeta(col_buf.CmaskAddress(), slice, false);
    }
    auto& image = texture_cache.GetImage(image_id);
    const auto clear_value = LiverpoolToVK::ColorBufferClearValue(col_buf);

    ScopeMarkerBegin(fmt::format("EliminateFastClear:MRT={:#x}:M={:#x}", col_buf.Address(),
                                 col_buf.CmaskAddress()));
    image.Clear(clear_value, desc.view_info.range);
    ScopeMarkerEnd();
}

void Rasterizer::Draw(bool is_indexed, u32 index_offset) {
    RENDERER_TRACE;

    scheduler.PopPendingOperations();
    buffer_cache.DrainRetiredBuffers();
    if (scheduler.IsFlushDue()) {
        Flush();
    }

    if (!FilterDraw()) {
        return;
    }
    const auto& regs = liverpool->regs;
    const GraphicsPipeline* pipeline = pipeline_cache.GetGraphicsPipeline();
    if (pipeline != nullptr && !in_target_passes) {
        if (const auto passes = SharedTargetPasses(pipeline->GetGraphicsKey().mrt_mask);
            passes.size() > 1) {
            // One surface as two of the draw's targets: a pass for each.
            in_target_passes = true;
            for (const u8 left_out : passes) {
                pipeline_cache.LeaveTargetsOut(left_out);
                Draw(is_indexed, index_offset);
            }
            pipeline_cache.LeaveTargetsOut(0);
            in_target_passes = false;
            return;
        }
    }
    // SHADPS4_DBG_SKIP_PS=<hash>[,<hash>...]: draws whose pixel shader is one of these are left
    // out, to see in the picture what they draw.
    static const std::vector<u64> skip_ps = [] {
        std::vector<u64> hashes;
        if (const char* value = std::getenv("SHADPS4_DBG_SKIP_PS"); value != nullptr) {
            for (const char* at = value; *at != '\0';) {
                char* end = nullptr;
                hashes.push_back(std::strtoull(at, &end, 16));
                at = (*end == ',') ? end + 1 : end;
                if (end == at && *end != '\0') {
                    break;
                }
            }
        }
        return hashes;
    }();
    if (!skip_ps.empty() && pipeline != nullptr &&
        pipeline->GetStages()[static_cast<u32>(Shader::LogicalStage::Fragment)] != nullptr &&
        std::ranges::find(skip_ps,
                          pipeline->GetStage(Shader::LogicalStage::Fragment).pgm_hash) !=
            skip_ps.end()) {
        return;
    }
    const bool trace = TraceDraw();
    if (trace) {
        static const GraphicsPipelineKey empty_key{};
        const auto& key = pipeline ? pipeline->GetGraphicsKey() : empty_key;
        const auto& vp = regs.viewports[0];
        const auto program_hash = [&](Shader::LogicalStage stage) -> u64 {
            return pipeline && pipeline->GetStages()[static_cast<u32>(stage)]
                       ? pipeline->GetStage(stage).pgm_hash : 0;
        };
        LOG_INFO(Render_Vulkan,
                 "TRACE draw idx={} n={} inst={} prim={} pipeline={} cb0={:#x} {}x{} s{} "
                 "cb_mode={} mask={:#x} db={:#x} z_en={} z_wr={} zfunc={} samples={}/{} "
                 "vp=({:.1f},{:.1f},{:.1f})+({:.1f},{:.1f},{:.1f}) vs={:#x} ps={:#x} st_en={} "
                 "st_func={} st_ref={} st_mask={:#x} st_wmask={:#x} zclear={} sclear={} "
                 "depth_clear={} htile={:#x} htile_cleared={} zfmt={} stencil_clear={} "
                 "depth_bounds={}/{}/{} stencil_ops={}/{}/{} db_control={:#x} "
                 "stencil_addr={:#x}/{:#x} back_stencil={}/{}/{} shader_depth={:#x} "
                 "vs_program={:#x} ps_program={:#x} clipper={:#x} depth_clip={} depth_clamp={}",
                 is_indexed, regs.num_indices, regs.num_instances.NumInstances(),
                 static_cast<u32>(regs.primitive_type), pipeline != nullptr,
                 regs.color_buffers[0].Address(), regs.color_buffers[0].Pitch(),
                 regs.color_buffers[0].Height(), regs.color_buffers[0].NumSamples(),
                 static_cast<u32>(regs.color_control.mode), regs.color_target_mask.raw,
                 regs.depth_buffer.DepthAddress(),
                 static_cast<bool>(regs.depth_control.depth_enable),
                 static_cast<bool>(regs.depth_control.depth_write_enable),
                 static_cast<u32>(regs.depth_control.depth_func), key.num_samples,
                 key.depth_samples, vp.xscale, vp.yscale, vp.zscale, vp.xoffset, vp.yoffset,
                 vp.zoffset, key.stage_hashes[static_cast<u32>(Shader::LogicalStage::Vertex)],
                 key.stage_hashes[static_cast<u32>(Shader::LogicalStage::Fragment)],
                 static_cast<bool>(regs.depth_control.stencil_enable),
                 static_cast<u32>(regs.depth_control.stencil_ref_func),
                 static_cast<u32>(regs.stencil_ref_front.stencil_test_val),
                 static_cast<u32>(regs.stencil_ref_front.stencil_mask),
                 static_cast<u32>(regs.stencil_ref_front.stencil_write_mask),
                 static_cast<bool>(regs.depth_render_control.depth_clear_enable),
                 static_cast<bool>(regs.depth_render_control.stencil_clear_enable),
                 regs.depth_clear, regs.depth_htile_data_base.GetAddress(),
                 texture_cache.IsMetaCleared(regs.depth_htile_data_base.GetAddress(),
                                             regs.depth_view.slice_start),
                 static_cast<u32>(regs.depth_buffer.z_info.format), regs.stencil_clear,
                 static_cast<bool>(regs.depth_control.depth_bounds_enable),
                 regs.depth_bounds_min, regs.depth_bounds_max,
                 static_cast<u32>(regs.stencil_control.stencil_fail_front),
                 static_cast<u32>(regs.stencil_control.stencil_zpass_front),
                 static_cast<u32>(regs.stencil_control.stencil_zfail_front),
                 std::bit_cast<u32>(regs.depth_control), regs.depth_buffer.StencilAddress(),
                 regs.depth_buffer.StencilWriteAddress(),
                 static_cast<bool>(regs.depth_control.backface_enable),
                 static_cast<u32>(regs.depth_control.stencil_bf_func),
                 static_cast<u32>(regs.stencil_ref_back.stencil_test_val),
                 std::bit_cast<u32>(regs.depth_shader_control),
                 program_hash(Shader::LogicalStage::Vertex),
                 program_hash(Shader::LogicalStage::Fragment),
                 std::bit_cast<u32>(regs.clipper_control), key.depth_clip_enable, key.depth_clamp_enable);
    }
    if (!pipeline) {
        return;
    }

    static const u64 capture_shader = [] {
        const char* value = std::getenv("SHADPS4_DUMP_BOUND_DRAW_BUFFERS");
        return value ? std::strtoull(value, nullptr, 16) : 0ULL;
    }();
    if (!trace) captured_draw_batch_count = 0;
    const auto& capture_vs = pipeline->GetStage(Shader::LogicalStage::Vertex);
    capture_draw_buffers = capture_shader && trace &&
        capture_vs.pgm_hash == capture_shader && captured_draw_batch_count < 12;
    draw_buffer_snapshots.clear();

    PrepareRenderState(pipeline);
    if (!BindResources(pipeline)) {
        if (trace) {
            LOG_INFO(Render_Vulkan, "TRACE   draw dropped: resources could not be bound");
        }
        return;
    }
    const auto state = BeginRendering(pipeline);

    buffer_cache.BindVertexBuffers(*pipeline);
    if (is_indexed) {
        const auto bound_index = buffer_cache.BindIndexBuffer(index_offset, capture_draw_buffers);
        if (capture_draw_buffers) {
            const u32 index_size = regs.index_buffer_type.index_type == AmdGpu::IndexType::Index16 ? 2 : 4;
            draw_buffer_snapshots.push_back({bound_index,
                regs.index_base_address.Address<VAddr>() + index_offset * index_size,
                fmt::format("index{}", index_size * 8)});
        }
    }

    pipeline->BindResources(set_writes, buffer_barriers, push_data);
    UpdateDynamicState(pipeline, is_indexed);
    scheduler.BeginRendering(state);
    scheduler.NoteDraw();
    FrameStats::Draw();

    const auto& vs_info = pipeline->GetStage(Shader::LogicalStage::Vertex);
    const auto& fetch_shader = pipeline->GetFetchShader();
    const auto [vertex_offset, instance_offset] = GetDrawOffsets(regs, vs_info, fetch_shader);

    const auto cmdbuf = scheduler.CommandBuffer();
    scheduler.BindGraphicsPipeline(pipeline->Handle());

    buffer_cache.SetVertexInput(*pipeline);

    // SHADPS4_DBG_DRAW_VERTICES=<count> draws no more than that of every draw: with next to
    // no geometry left, what a frame still costs is what its draws cost as such.
    static const u32 vertex_limit = [] {
        const char* value = std::getenv("SHADPS4_DBG_DRAW_VERTICES");
        return value != nullptr && value[0] != 0 ? static_cast<u32>(std::atoi(value)) : ~0u;
    }();
    const u32 num_vertices = std::min<u32>(regs.num_indices, vertex_limit);
    FrameStats::Add(FrameStats::Counter::Vertices,
                    u64{num_vertices} * regs.num_instances.NumInstances());

    if (is_indexed) {
        cmdbuf.drawIndexed(num_vertices, regs.num_instances.NumInstances(), 0,
                           s32(vertex_offset), instance_offset);
    } else {
        cmdbuf.draw(num_vertices, regs.num_instances.NumInstances(), vertex_offset,
                    instance_offset);
    }

    if (capture_draw_buffers) {
        LOG_INFO(Render_Vulkan, "DRAW_BUFFER_CAPTURE draw={} shader={:#x} indexed={} count={} instances={} vertex_offset={} instance_offset={}",
                 captured_draw_count, capture_shader, is_indexed, num_vertices,
                 regs.num_instances.NumInstances(), s32(vertex_offset), instance_offset);
        DumpBoundDrawBuffers();
        capture_draw_buffers = false;
        ++captured_draw_count;
        ++captured_draw_batch_count;
    }
    ResetBindings();
    RetireIsolatedReadConstSnapshots();
}

boost::container::small_vector<u8, 4> Rasterizer::SharedTargetPasses(u8 mrt_mask) const {
    // A title may bind one surface as two colour targets and write some channels through the
    // one, the others through the other: that way each can have blending of its own. (Astro
    // Bot draws the rings on its lava like that, colour blended in through target 0 and
    // alpha written through target 1; both are the same quarter-size buffer.) The console's
    // GPU writes both to the one memory. Vulkan leaves undefined what an image holds that
    // was two attachments of one render pass, and on the desktop GPU this was found on it
    // held the draw in some blocks of 4x8 texels and not in others: squares in the lava's
    // glow around the octopus of world 2. Such a draw is made once for every target that
    // shares a surface, with the others left out: what each pass writes is what the one
    // draw would have.
    const auto& regs = liverpool->regs;
    boost::container::small_vector<u8, 4> passes;
    passes.push_back(0);
    if (regs.color_control.mode == AmdGpu::ColorControl::OperationMode::Disable) {
        return passes;
    }
    u8 used = 0;
    for (u32 cb = 0; cb < AmdGpu::NUM_COLOR_BUFFERS; ++cb) {
        if ((mrt_mask & (1u << cb)) != 0 && regs.color_buffers[cb] &&
            regs.color_target_mask.GetMask(cb) != 0) {
            used |= 1u << cb;
        }
    }
    const auto same_surface = [&](u32 a, u32 b) {
        const auto& first = regs.color_buffers[a];
        const auto& second = regs.color_buffers[b];
        return first.Address() == second.Address() &&
               first.view.slice_start == second.view.slice_start;
    };
    // Every target goes into the first pass that has no target of the same surface yet.
    boost::container::small_vector<u8, 4> drawn;
    for (u32 cb = 0; cb < AmdGpu::NUM_COLOR_BUFFERS; ++cb) {
        if ((used & (1u << cb)) == 0) {
            continue;
        }
        u32 pass = 0;
        for (; pass < drawn.size(); ++pass) {
            bool shared = false;
            for (u32 other = 0; other < cb; ++other) {
                shared |= (drawn[pass] & (1u << other)) != 0 && same_surface(cb, other);
            }
            if (!shared) {
                break;
            }
        }
        if (pass == drawn.size()) {
            drawn.push_back(0);
        }
        drawn[pass] |= 1u << cb;
    }
    if (drawn.size() <= 1) {
        return passes;
    }
    // What a draw does to depth and stencil it would do once for every pass. That is the same
    // only where a second pass passes the depth test where the first did and writes the same
    // depth again, and where nothing counts in the stencil.
    const auto& depth = regs.depth_control;
    const bool writes_depth = depth.depth_enable && depth.depth_write_enable;
    const bool repeatable = depth.depth_func == AmdGpu::CompareFunc::LessEqual ||
                            depth.depth_func == AmdGpu::CompareFunc::GreaterEqual ||
                            depth.depth_func == AmdGpu::CompareFunc::Equal ||
                            depth.depth_func == AmdGpu::CompareFunc::Always;
    if (depth.stencil_enable || (writes_depth && !repeatable)) {
        static bool told = false;
        if (!std::exchange(told, true)) {
            LOG_WARNING(Render_Vulkan,
                        "A draw has one surface ({:#x}) as more than one colour target and "
                        "writes depth or stencil in a way that cannot be done twice: it is "
                        "drawn in one pass, and the picture may have squares where it draws",
                        regs.color_buffers[std::countr_zero(used)].Address());
        }
        return passes;
    }
    passes.clear();
    for (const u8 targets : drawn) {
        passes.push_back(used & ~targets);
    }
    return passes;
}

void Rasterizer::DrawIndirect(bool is_indexed, VAddr arg_address, u32 offset, u32 stride,
                              u32 max_count, VAddr count_address) {
    RENDERER_TRACE;

    if (TraceDraw()) {
        const auto& regs = liverpool->regs;
        LOG_INFO(Render_Vulkan,
                 "TRACE indirect idx={} args={:#x} stride={} max={} count={:#x} cb0={:#x} "
                 "mask={:#x} db={:#x} db_control={:#x} stencil_control={:#x} "
                 "stencil_ref={:#x} zclear={} sclear={}",
                 is_indexed, arg_address + offset, stride, max_count, count_address,
                 regs.color_buffers[0].Address(), regs.color_target_mask.raw,
                 regs.depth_buffer.DepthAddress(), std::bit_cast<u32>(regs.depth_control),
                 std::bit_cast<u32>(regs.stencil_control),
                 std::bit_cast<u32>(regs.stencil_ref_front),
                 static_cast<bool>(regs.depth_render_control.depth_clear_enable),
                 static_cast<bool>(regs.depth_render_control.stencil_clear_enable));
    }

    scheduler.PopPendingOperations();
    buffer_cache.DrainRetiredBuffers();
    if (scheduler.IsFlushDue()) {
        Flush();
    }

    if (!FilterDraw()) {
        return;
    }

    const GraphicsPipeline* pipeline = pipeline_cache.GetGraphicsPipeline();
    if (!pipeline) {
        return;
    }

    PrepareRenderState(pipeline);
    if (!BindResources(pipeline)) {
        return;
    }
    const auto state = BeginRendering(pipeline);

    buffer_cache.BindVertexBuffers(*pipeline);
    if (is_indexed) {
        buffer_cache.BindIndexBuffer(0);
    }

    const auto& [buffer, base] =
        buffer_cache.ObtainBuffer(arg_address + offset, stride * max_count, false);

    VideoCore::Buffer* count_buffer{};
    u32 count_base{};
    if (count_address != 0) {
        std::tie(count_buffer, count_base) = buffer_cache.ObtainBuffer(count_address, 4, false);
    }

    pipeline->BindResources(set_writes, buffer_barriers, push_data);
    UpdateDynamicState(pipeline, is_indexed);
    scheduler.BeginRendering(state);
    scheduler.NoteDraw();
    FrameStats::Draw();

    // We can safely ignore both SGPR UD indices and results of fetch shader parsing, as vertex and
    // instance offsets will be automatically applied by Vulkan from indirect args buffer.

    const auto cmdbuf = scheduler.CommandBuffer();
    scheduler.BindGraphicsPipeline(pipeline->Handle());

    buffer_cache.SetVertexInput(*pipeline);

    if (is_indexed) {
        ASSERT(sizeof(VkDrawIndexedIndirectCommand) == stride);

        if (count_address != 0) {
            cmdbuf.drawIndexedIndirectCount(buffer->Handle(), base, count_buffer->Handle(),
                                            count_base, max_count, stride);
        } else {
            cmdbuf.drawIndexedIndirect(buffer->Handle(), base, max_count, stride);
        }
    } else {
        ASSERT(sizeof(VkDrawIndirectCommand) == stride);

        if (count_address != 0) {
            cmdbuf.drawIndirectCount(buffer->Handle(), base, count_buffer->Handle(), count_base,
                                     max_count, stride);
        } else {
            cmdbuf.drawIndirect(buffer->Handle(), base, max_count, stride);
        }
    }

    ResetBindings();
    RetireIsolatedReadConstSnapshots();
}

void Rasterizer::DispatchDirect() {
    RENDERER_TRACE;

    scheduler.PopPendingOperations();
    buffer_cache.DrainRetiredBuffers();
    if (scheduler.IsFlushDue()) {
        Flush();
    }
    FrameStats::Compute();

    const auto& cs_program = liverpool->GetCsRegs();
    const ComputePipeline* pipeline = pipeline_cache.GetComputePipeline();
    const bool trace = TraceDraw();
    if (trace) {
        LOG_INFO(Render_Vulkan, "TRACE dispatch {}x{}x{} pipeline={} cs_program={:#x}", cs_program.dim_x,
                 cs_program.dim_y, cs_program.dim_z, pipeline != nullptr,
                 pipeline ? pipeline->GetStage(Shader::LogicalStage::Compute).pgm_hash : 0);
    }
    if (!pipeline) {
        return;
    }

    const auto& cs = pipeline->GetStage(Shader::LogicalStage::Compute);
    if (ExecuteShaderHLE(cs, liverpool->regs, cs_program, *this)) {
        if (trace) {
            LOG_INFO(Render_Vulkan, "TRACE   dispatch handled by shader HLE");
        }
        return;
    }

    const bool capture_compute = BeginComputeBufferCapture(cs);

    if (!BindResources(pipeline)) {
        if (trace) {
            LOG_INFO(Render_Vulkan, "TRACE   dispatch dropped by BindResources");
        }
        capture_draw_buffers = false;
        return;
    }

    scheduler.EndRendering();
    pipeline->BindResources(set_writes, buffer_barriers, push_data);

    if (capture_compute) DumpBoundDrawBuffers("before-");

    const auto cmdbuf = scheduler.CommandBuffer();
    cmdbuf.bindPipeline(vk::PipelineBindPoint::eCompute, pipeline->Handle());
    cmdbuf.dispatch(cs_program.dim_x, cs_program.dim_y, cs_program.dim_z);

    if (capture_compute) {
        DumpBoundDrawBuffers("after-");
        capture_draw_buffers = false;
        ++captured_draw_count;
    }

    // Raw relocation copies can overwrite textures contained in
    // a larger allocation. The formatted-buffer invalidation path misses them.
    // Restrict the correction to Rush's observed 16-byte copy shader, and require
    // complete image coverage; never replace partial GPU images with CPU data.
    static const bool rush_copy_images = [] {
        const char* value = std::getenv("SHADPS4_TEST_RUSH_COPY_IMAGES");
        return (!value || std::string_view{value} != "0") &&
               Common::ElfInfo::Instance().GameSerial() == "CUSA02350";
    }();
    if (rush_copy_images && cs.pgm_hash == 0xaf64ca41) {
        for (const auto& desc : cs.buffers) {
            if (desc.IsSpecial() || !desc.is_written) continue;
            const auto sharp = desc.GetSharp(cs);
            const u64 bytes = u64{cs_program.user_data[2]} * 16;
            if (sharp.base_address && bytes && bytes <= sharp.GetSize()) {
                texture_cache.InvalidateImagesCoveredByGpuCopy(sharp.base_address, bytes);
            }
        }
    }

    // Research control: publish the real GPU result to guest memory before the
    // command processor can signal completion. This is intentionally synchronous
    // and must not be confused with the separate CPU-source override experiment.
    static const bool rush_copy_readback_trial = [] {
        const char* value = std::getenv("SHADPS4_TEST_RUSH_COPY_READBACK");
        return value && std::string_view{value} == "1" &&
               Common::ElfInfo::Instance().GameSerial() == "CUSA02350";
    }();
    if (rush_copy_readback_trial && cs.pgm_hash == 0xaf64ca41) {
        for (const auto& desc : cs.buffers) {
            if (desc.IsSpecial() || !desc.is_written) continue;
            const auto sharp = desc.GetSharp(cs);
            const u64 size = std::min<u64>(sharp.GetSize(), u64{cs_program.user_data[2]} * 16);
            if (sharp.base_address && size) {
                buffer_cache.ReadMemory(sharp.base_address, size);
            }
        }
    }

    ResetBindings();
}

void Rasterizer::DispatchIndirect(VAddr address, u32 offset, u32 size) {
    RENDERER_TRACE;

    scheduler.PopPendingOperations();
    buffer_cache.DrainRetiredBuffers();
    if (scheduler.IsFlushDue()) {
        Flush();
    }

    const auto& cs_program = liverpool->GetCsRegs();
    const ComputePipeline* pipeline = pipeline_cache.GetComputePipeline();
    if (!pipeline) {
        return;
    }

    if (!BindResources(pipeline)) {
        return;
    }

    const auto [buffer, base] = buffer_cache.ObtainBuffer(address + offset, size, false);

    scheduler.EndRendering();
    pipeline->BindResources(set_writes, buffer_barriers, push_data);

    const auto cmdbuf = scheduler.CommandBuffer();
    cmdbuf.bindPipeline(vk::PipelineBindPoint::eCompute, pipeline->Handle());
    cmdbuf.dispatchIndirect(buffer->Handle(), base);

    ResetBindings();
}

u64 Rasterizer::Flush() {
    const u64 current_tick = scheduler.CurrentTick();
    SubmitInfo info{};
    scheduler.Flush(info);
    return current_tick;
}

void Rasterizer::Finish() {
    scheduler.Finish();
}

void Rasterizer::CompleteGuestReadbacks() {
    if (Common::ElfInfo::Instance().GameSerial() == "CUSA04508") {
        texture_cache.CompleteBrightnessReadbacks();
    }
}

void Rasterizer::OnSubmit() {
    if (fault_process_pending) {
        fault_process_pending = false;
        buffer_cache.ProcessFaultBuffer();
    }
    texture_cache.ProcessDownloadImages();
    texture_cache.RunGarbageCollector();
    buffer_cache.RunGarbageCollector();
}

bool Rasterizer::BindResources(const Pipeline* pipeline) {
    if (IsComputeImageCopy(pipeline)) {
        if (g_draw_trace_budget.load(std::memory_order_relaxed) > 0) {
            LOG_INFO(Render_Vulkan, "TRACE   compute handled as image copy");
        }
        return false;
    }
    if (IsComputeMetaClear(pipeline)) {
        if (g_draw_trace_budget.load(std::memory_order_relaxed) > 0) {
            LOG_INFO(Render_Vulkan, "TRACE   compute handled as meta clear");
        }
        return false;
    }
    if (IsComputeImageClear(pipeline)) {
        if (g_draw_trace_budget.load(std::memory_order_relaxed) > 0) {
            LOG_INFO(Render_Vulkan, "TRACE   compute handled as image clear");
        }
        return false;
    }

    set_write_index = 0;
    set_writes.clear();
    buffer_barriers.clear();
    buffer_infos.clear();
    image_infos.clear();

    bool uses_dma = false;

    // Bind resource buffers and textures.
    Shader::Backend::Bindings binding{};
    push_data = MakeUserData(liverpool->regs);
    for (const auto* stage : pipeline->GetStages()) {
        if (!stage) {
            continue;
        }
        set_writes.resize(set_writes.size() + stage->buffers.size() + stage->images.size() +
                          stage->samplers.size());
        stage->PushUd(binding, push_data);
        BindBuffers(*stage, binding, push_data);
        BindTextures(*stage, binding);
        uses_dma |= stage->uses_dma;
    }

    if (uses_dma) {
        // We only use fault buffer for DMA right now.
        Common::RecursiveSharedLock lock{mapped_ranges_mutex};
        for (auto& range : mapped_ranges) {
            buffer_cache.SynchronizeBuffersInRange(range.lower(), range.upper() - range.lower());
        }
        fault_process_pending = true;
    }

    return true;
}

bool Rasterizer::IsComputeMetaClear(const Pipeline* pipeline) {
    if (!pipeline->IsCompute()) {
        return false;
    }

    // Most of the time when a metadata is updated with a shader it gets cleared. It means
    // we can skip the whole dispatch and update the tracked state instead. Also, it is not
    // intended to be consumed and in such rare cases (e.g. HTile introspection, CRAA) we
    // will need its full emulation anyways.
    const auto& info = pipeline->GetStage(Shader::LogicalStage::Compute);

    if (g_draw_trace_budget.load(std::memory_order_relaxed) > 0) {
        for (const auto& desc : info.buffers) {
            const auto sharp = desc.GetSharp(info);
            LOG_INFO(Render_Vulkan,
                     "TRACE   cs buffer base={:#x} size={:#x} written={} special={} is_meta={} xor={}",
                     sharp.base_address, sharp.GetSize(), static_cast<bool>(desc.is_written),
                     desc.IsSpecial(), texture_cache.IsMeta(sharp.base_address),
                     static_cast<bool>(info.has_bitwise_xor));
        }
    }

    // Assume if a shader reads metadata, it is a copy shader.
    for (const auto& desc : info.buffers) {
        const VAddr address = desc.GetSharp(info).base_address;
        if (!desc.IsSpecial() && !desc.is_written && texture_cache.IsMeta(address)) {
            return false;
        }
    }

    // Metadata surfaces are tiled and thus need address calculation to be written properly.
    // If a shader wants to encode HTILE, for example, from a depth image it will have to compute
    // proper tile address from dispatch invocation id. This address calculation contains an xor
    // operation so use it as a heuristic for metadata writes that are probably not clears.
    if (!info.has_bitwise_xor) {
        // Assume if a shader writes metadata without address calculation, it is a clear shader.
        for (const auto& desc : info.buffers) {
            const VAddr address = desc.GetSharp(info).base_address;
            if (!desc.IsSpecial() && desc.is_written && texture_cache.ClearMeta(address)) {
                // Assume all slices were updates
                LOG_TRACE(Render_Vulkan, "Metadata update skipped");
                return true;
            }
        }
    }
    return false;
}

bool Rasterizer::IsComputeImageCopy(const Pipeline* pipeline) {
    if (!pipeline->IsCompute()) {
        return false;
    }

    // Ensure shader only has 2 bound buffers
    const auto& cs_pgm = liverpool->GetCsRegs();
    const auto& info = pipeline->GetStage(Shader::LogicalStage::Compute);
    if (cs_pgm.num_thread_x.full != 64 || info.buffers.size() != 2 || !info.images.empty()) {
        return false;
    }

    // Those 2 buffers must both be formatted. One must be source and another destination.
    const auto& desc0 = info.buffers[0];
    const auto& desc1 = info.buffers[1];
    if (!desc0.is_formatted || !desc1.is_formatted || desc0.is_written == desc1.is_written) {
        return false;
    }

    // Buffers must have the same size and each thread of the dispatch must copy 1 dword of data
    const AmdGpu::Buffer buf0 = desc0.GetSharp(info);
    const AmdGpu::Buffer buf1 = desc1.GetSharp(info);
    if (buf0.GetSize() != buf1.GetSize() || cs_pgm.dim_x != (buf0.GetSize() / 256)) {
        return false;
    }

    // Find images the buffer alias
    const auto image0_id = texture_cache.FindImageFromRange(buf0.base_address, buf0.GetSize());
    if (!image0_id) {
        return false;
    }
    const auto image1_id =
        texture_cache.FindImageFromRange(buf1.base_address, buf1.GetSize(), false);
    if (!image1_id) {
        return false;
    }

    // Image copy must be valid
    VideoCore::Image& image0 = texture_cache.GetImage(image0_id);
    VideoCore::Image& image1 = texture_cache.GetImage(image1_id);
    if (image0.info.guest_size != image1.info.guest_size ||
        image0.info.pitch != image1.info.pitch || image0.info.guest_size != buf0.GetSize() ||
        image0.info.num_bits != image1.info.num_bits) {
        return false;
    }

    // Perform image copy
    VideoCore::Image& src_image = desc0.is_written ? image1 : image0;
    VideoCore::Image& dst_image = desc0.is_written ? image0 : image1;
    if (instance.IsMaintenance8Supported() ||
        src_image.info.props.is_depth == dst_image.info.props.is_depth) {
        dst_image.CopyImage(src_image);
    } else {
        const auto& copy_buffer =
            buffer_cache.GetUtilityBuffer(VideoCore::MemoryUsage::DeviceLocal);
        dst_image.CopyImageWithBuffer(src_image, copy_buffer.Handle(), 0);
    }
    dst_image.flags |= VideoCore::ImageFlagBits::GpuModified;
    dst_image.flags &= ~VideoCore::ImageFlagBits::Dirty;
    return true;
}

bool Rasterizer::IsComputeImageClear(const Pipeline* pipeline) {
    if (!pipeline->IsCompute()) {
        return false;
    }

    // Ensure shader only has 2 bound buffers
    const auto& cs_pgm = liverpool->GetCsRegs();
    const auto& info = pipeline->GetStage(Shader::LogicalStage::Compute);
    if (cs_pgm.num_thread_x.full != 64 || info.buffers.size() != 2 || !info.images.empty()) {
        return false;
    }

    // From those 2 buffers, first must hold the clear vector and second the image being cleared
    const auto& desc0 = info.buffers[0];
    const auto& desc1 = info.buffers[1];
    if (desc0.is_formatted || !desc1.is_formatted || desc0.is_written || !desc1.is_written) {
        return false;
    }

    // First buffer must have size of vec4 and second the size of a single layer
    const AmdGpu::Buffer buf0 = desc0.GetSharp(info);
    const AmdGpu::Buffer buf1 = desc1.GetSharp(info);
    const u32 buf1_bpp = AmdGpu::NumBitsPerBlock(buf1.GetDataFmt());
    if (buf0.GetSize() != 16 || (cs_pgm.dim_x * 128ULL * (buf1_bpp / 8)) != buf1.GetSize()) {
        return false;
    }

    // Find image the buffer alias
    const auto image1_id =
        texture_cache.FindImageFromRange(buf1.base_address, buf1.GetSize(), false);
    if (!image1_id) {
        return false;
    }

    // Image clear must be valid
    VideoCore::Image& image1 = texture_cache.GetImage(image1_id);
    if (image1.info.guest_size != buf1.GetSize() || image1.info.num_bits != buf1_bpp ||
        image1.info.props.is_depth) {
        return false;
    }

    // Perform image clear
    const float* values = reinterpret_cast<float*>(buf0.base_address);
    const vk::ClearValue clear = {
        .color = {.float32 = std::array<float, 4>{values[0], values[1], values[2], values[3]}},
    };
    const VideoCore::SubresourceRange range = {
        .base =
            {
                .level = 0,
                .layer = 0,
            },
        .extent = image1.info.resources,
    };
    image1.Clear(clear, range);
    image1.flags |= VideoCore::ImageFlagBits::GpuModified;
    image1.flags &= ~VideoCore::ImageFlagBits::Dirty;
    return true;
}

void Rasterizer::BindBuffers(const Shader::Info& stage, Shader::Backend::Bindings& binding,
                             Shader::PushData& push_data) {
    buffer_bindings.clear();
    // Some scalar loads consume constants previously produced as a tiny render target.
    // The normal image-to-buffer path is restricted to formatted buffer loads.
    // Keep this trial bounded to small, unambiguous single-sample color images.
    const auto readconst_image = [&](const auto& desc, const AmdGpu::Buffer& sharp) -> VideoCore::ImageId {
        static const bool enabled = [] {
            const char* trial = std::getenv("SHADPS4_TEST_READCONST_IMAGE_SYNC");
            return trial && std::string_view{trial} == "1";
        }();
        if (!enabled || !desc.used_as_readconst ||
            desc.IsSpecial() || desc.is_written || !sharp.base_address || !sharp.GetSize()) {
            return {};
        }
        const auto id = texture_cache.FindImageFromRange(sharp.base_address, sharp.GetSize());
        if (!id) {
            return {};
        }
        const auto& info = texture_cache.GetImage(id).info;
        if (info.props.is_depth || info.props.is_volume || info.num_samples != 1 ||
            info.resources.layers != 1 || info.resources.levels != 1 ||
            info.size.width > 64 || info.size.height > 64 || info.guest_size > 0x10000 ||
            memory->ClampRangeSize(sharp.base_address, info.guest_size) != info.guest_size) {
            return {};
        }
        return id;
    };

    // These options are fixed by the launcher before process creation. Avoid
    // repeated CRT environment scans for every buffer of every draw.
    static const std::optional<u64> traced_shader = []() -> std::optional<u64> {
        const char* selected = std::getenv("SHADPS4_TRACE_SHADER_BUFFERS");
        if (!selected) return std::nullopt;
        return std::strtoull(selected, nullptr, 16);
    }();
    static const bool null_unmapped = std::getenv("SHADPS4_TEST_NULL_UNMAPPED_BUFFERS") != nullptr;
    static const bool rush_copy_cpu_trial = [] {
        const char* value = std::getenv("SHADPS4_TEST_RUSH_COPY_CPU_SOURCE");
        return value && std::string_view{value} == "1" &&
               Common::ElfInfo::Instance().GameSerial() == "CUSA02350";
    }();
    static const bool trace_buffers_on_command =
        std::getenv("SHADPS4_TRACE_SHADER_BUFFERS_ON_DRAW_TRACE") != nullptr;
    static u32 buffer_reports = 0;
    const bool trace_buffers_now = !trace_buffers_on_command ||
                                  g_draw_trace_budget.load(std::memory_order_relaxed) > 0;
    if (!trace_buffers_now) buffer_reports = 0;
    // Keep both ends on one line: adjacent descriptor messages can belong to
    // different dispatches when the guest stages a relocation through a scratch buffer.
    if (stage.stage == Shader::Stage::Compute && stage.pgm_hash == 0xaf64ca41) {
        for (const auto& source : stage.buffers) {
            if (source.IsSpecial() || source.is_written) continue;
            const auto input = source.GetSharp(stage);
            for (const auto& destination : stage.buffers) {
                if (destination.IsSpecial() || !destination.is_written) continue;
                const auto output = destination.GetSharp(stage);
                if (!TraceBufferWrite(input.base_address, input.GetSize()) &&
                    !TraceBufferWrite(output.base_address, output.GetSize())) continue;
                LOG_INFO(Render_Vulkan, "COPY_DISPATCH source={:#x} source_bytes={} destination={:#x} destination_bytes={} blocks={} tick={}",
                         static_cast<u64>(input.base_address), input.GetSize(),
                         static_cast<u64>(output.base_address), output.GetSize(),
                         liverpool->GetCsRegs().user_data[2], scheduler.CurrentTick());
            }
        }
    }
    for (const auto& desc : stage.buffers) {
        const auto vsharp = desc.GetSharp(stage);
        if (stage.stage == Shader::Stage::Compute && stage.pgm_hash == 0xaf64ca41 &&
            !desc.IsSpecial() && !desc.is_written &&
            TraceBufferWrite(vsharp.base_address, vsharp.GetSize())) {
            LOG_INFO(Render_Vulkan, "COPY_SOURCE_STATE address={:#x} bytes={} cpu_dirty={} gpu_dirty={} tick={}",
                     static_cast<u64>(vsharp.base_address), vsharp.GetSize(),
                     buffer_cache.IsRegionCpuModified(vsharp.base_address, vsharp.GetSize()),
                     buffer_cache.IsRegionGpuModified(vsharp.base_address, vsharp.GetSize()),
                     scheduler.CurrentTick());
        }
        if (desc.is_written && TraceBufferWrite(desc.IsSpecial() ? 0 : vsharp.base_address,
                                                desc.IsSpecial() ? 1 : vsharp.GetSize())) {
            const auto& cs_regs = liverpool->GetCsRegs();
            LOG_INFO(Render_Vulkan, "BUFFER_WRITE shader={:#x} stage={} sharp={} address={:#x} bytes={} stride={} format={}/{} special={} groups={}x{}x{} tick={}",
                     stage.pgm_hash, static_cast<u32>(stage.stage), desc.sharp_idx,
                     static_cast<u64>(vsharp.base_address), vsharp.GetSize(), vsharp.GetStride(),
                     static_cast<u32>(vsharp.GetDataFmt()), static_cast<u32>(vsharp.GetNumberFmt()),
                     desc.IsSpecial(), cs_regs.dim_x, cs_regs.dim_y, cs_regs.dim_z, scheduler.CurrentTick());
        }
        if (traced_shader && stage.pgm_hash == *traced_shader && trace_buffers_now) {
            if (buffer_reports++ < 80) {
                std::array<u32, 8> words{};
                const bool readable = !desc.IsSpecial() && vsharp.base_address &&
                    memory->IsValidMapping(vsharp.base_address) &&
                    memory->ClampRangeSize(vsharp.base_address, vsharp.GetSize()) >= sizeof(words);
                if (readable) {
                    std::memcpy(words.data(), reinterpret_cast<const void*>(vsharp.base_address), sizeof(words));
                }
                LOG_INFO(Render_Vulkan,
                         "SHADER_BUFFER shader={:#x} sharp={} addr={:#x} size={} special={} readconst={} readable={} stride={} format={}/{} gpu_modified={} words={:08x},{:08x},{:08x},{:08x},{:08x},{:08x},{:08x},{:08x}",
                         stage.pgm_hash, desc.sharp_idx, static_cast<u64>(vsharp.base_address),
                         vsharp.GetSize(), desc.IsSpecial(), desc.used_as_readconst, readable,
                         vsharp.GetStride(), static_cast<u32>(vsharp.GetDataFmt()),
                         static_cast<u32>(vsharp.GetNumberFmt()),
                         readable && buffer_cache.IsRegionGpuModified(vsharp.base_address, vsharp.GetSize()),
                         words[0], words[1], words[2], words[3], words[4], words[5], words[6], words[7]);
                if (readable && desc.used_as_readconst && buffer_reports <= 16) {
                    const auto bytes = std::min<u64>(vsharp.GetSize(), 1024);
                    if (memory->ClampRangeSize(vsharp.base_address, bytes) == bytes) {
                        std::string constants;
                        for (u64 i = 0; i < bytes / 4; ++i) {
                            u32 word;
                            std::memcpy(&word, reinterpret_cast<const void*>(vsharp.base_address + i * 4), 4);
                            constants += fmt::format("{:08x} ", word);
                        }
                        LOG_INFO(Render_Vulkan, "SHADER_CONSTANTS shader={:#x} sharp={} words={}",
                                 stage.pgm_hash, desc.sharp_idx, constants);
                    }
                }
            }
        }
        if (null_unmapped && !desc.IsSpecial() &&
            vsharp.base_address != 0 && !memory->IsValidMapping(vsharp.base_address)) {
            static u32 reports = 0;
            if (reports++ < 20) {
                LOG_WARNING(Render_Vulkan, "Diagnostic null binding: shader={:#x} sharp={} unmapped address={:#x}",
                            stage.pgm_hash, desc.sharp_idx, static_cast<u64>(vsharp.base_address));
            }
            buffer_bindings.emplace_back(VideoCore::BufferId{}, AmdGpu::Buffer::Null(), 0);
            continue;
        }
        if (!desc.IsSpecial() && vsharp.base_address != 0 && vsharp.base_address < 0x10000) {
            LOG_CRITICAL(Render_Vulkan, "Invalid small shader buffer: shader={:#x} stage={} sharp={} address={:#x} size={:#x} raw={:08x}/{:08x}/{:08x}/{:08x}",
                         stage.pgm_hash, static_cast<u32>(stage.stage), desc.sharp_idx,
                         static_cast<u64>(vsharp.base_address), vsharp.GetSize(),
                         reinterpret_cast<const u32*>(&vsharp)[0], reinterpret_cast<const u32*>(&vsharp)[1],
                         reinterpret_cast<const u32*>(&vsharp)[2], reinterpret_cast<const u32*>(&vsharp)[3]);
        }
        if (!desc.IsSpecial() && vsharp.base_address != 0 && vsharp.GetSize() > 0) {
            const u64 size = memory->ClampRangeSize(vsharp.base_address, vsharp.GetSize());
            // Research control only: the post-tutorial relocation copy has valid CPU
            // indices but stale GPU source data. Refresh its direct-memory source to
            // test that hypothesis; do not apply this policy to normal GPU resources.
            if (rush_copy_cpu_trial && stage.stage == Shader::Stage::Compute &&
                stage.pgm_hash == 0xaf64ca41 && !desc.is_written &&
                vsharp.base_address >= 0x1000000000ULL && scheduler.CurrentTick() >= 10000 &&
                EmulatorSettings.GetReadbacksMode() == GpuReadbacksMode::Disabled) {
                LOG_WARNING(Render_Vulkan, "RUSH_COPY_CPU_TRIAL address={:#x} bytes={} tick={}",
                            static_cast<u64>(vsharp.base_address), size, scheduler.CurrentTick());
                buffer_cache.InvalidateMemory(vsharp.base_address, size);
            }
            u64 allocation_size = size;
            if (const auto image_id = readconst_image(desc, vsharp)) {
                allocation_size = std::max<u64>(allocation_size, texture_cache.GetImage(image_id).info.guest_size);
                static u32 reports = 0;
                if (reports++ < 8) {
                    LOG_INFO(Render_Vulkan, "READCONST_IMAGE_SYNC shader={:#x} addr={:#x} descriptor={} image={}",
                             stage.pgm_hash, static_cast<u64>(vsharp.base_address), size, allocation_size);
                }
            }
            const auto buffer_id = buffer_cache.FindBuffer(vsharp.base_address, allocation_size);
            buffer_bindings.emplace_back(buffer_id, vsharp, size);
        } else {
            buffer_bindings.emplace_back(VideoCore::BufferId{}, vsharp, 0);
        }
    }

    // Second pass to re-bind buffers that were updated after binding
    for (u32 i = 0; i < buffer_bindings.size(); i++) {
        const auto& [buffer_id, vsharp, size] = buffer_bindings[i];
        const auto& desc = stage.buffers[i];
        const bool is_storage = desc.IsStorage(vsharp);
        const u32 alignment =
            is_storage ? instance.StorageMinAlignment() : instance.UniformMinAlignment();
        // Buffer is not from the cache, either a special buffer or unbound.
        if (ShouldIsolateReadConstBuffer(stage.pgm_hash, stage.stage, desc.used_as_readconst,
                                         desc.is_written) &&
            vsharp.base_address != 0 && size > 0) {
            auto isolated = IsolateReadConstGuestBuffer(vsharp.base_address, size);
            buffer_infos.emplace_back(isolated->Handle(), 0, size);
            buffer_barriers.emplace_back(vk::BufferMemoryBarrier2{
                .srcStageMask = vk::PipelineStageFlagBits2::eHost,
                .srcAccessMask = vk::AccessFlagBits2::eHostWrite,
                .dstStageMask = vk::PipelineStageFlagBits2::eAllCommands,
                .dstAccessMask = vk::AccessFlagBits2::eShaderRead,
                .buffer = isolated->Handle(),
                .offset = 0,
                .size = size,
            });
            isolated_readconst_buffers.push_back(std::move(isolated));
        } else if (!buffer_id) {
            if (desc.buffer_type == Shader::BufferType::GdsBuffer) {
                const auto* gds_buf = buffer_cache.GetGdsBuffer();
                buffer_infos.emplace_back(gds_buf->Handle(), 0, gds_buf->SizeBytes());
            } else if (desc.buffer_type == Shader::BufferType::Flatbuf) {
                auto& vk_buffer = buffer_cache.GetUtilityBuffer(VideoCore::MemoryUsage::Stream);
                const u32 ubo_size = stage.flattened_ud_buf.size() * sizeof(u32);
                const u64 offset =
                    vk_buffer.Copy(stage.flattened_ud_buf.data(), ubo_size, alignment);
                buffer_infos.emplace_back(vk_buffer.Handle(), offset, ubo_size);
            } else if (desc.buffer_type == Shader::BufferType::BdaPagetable) {
                const auto* bda_buffer = buffer_cache.GetBdaPageTableBuffer();
                buffer_infos.emplace_back(bda_buffer->Handle(), 0, bda_buffer->SizeBytes());
            } else if (desc.buffer_type == Shader::BufferType::FaultBuffer) {
                const auto* fault_buffer = buffer_cache.GetFaultBuffer();
                buffer_infos.emplace_back(fault_buffer->Handle(), 0, fault_buffer->SizeBytes());
            } else if (desc.buffer_type == Shader::BufferType::SharedMemory) {
                auto& lds_buffer = buffer_cache.GetUtilityBuffer(VideoCore::MemoryUsage::Stream);
                const auto& cs_program = liverpool->GetCsRegs();
                const auto lds_size = cs_program.SharedMemSize() * cs_program.NumWorkgroups();
                const auto [data, offset] = lds_buffer.Map(lds_size, alignment);
                std::memset(data, 0, lds_size);
                buffer_infos.emplace_back(lds_buffer.Handle(), offset, lds_size);
            } else if (instance.IsNullDescriptorSupported()) {
                buffer_infos.emplace_back(VK_NULL_HANDLE, 0, VK_WHOLE_SIZE);
            } else {
                auto& null_buffer = buffer_cache.GetBuffer(VideoCore::NULL_BUFFER_ID);
                buffer_infos.emplace_back(null_buffer.Handle(), 0, VK_WHOLE_SIZE);
            }
        } else {
            const auto [vk_buffer, offset] = buffer_cache.ObtainBuffer(
                vsharp.base_address, size, desc.is_written, desc.is_formatted, buffer_id,
                bool(readconst_image(desc, vsharp)));
            const u32 offset_aligned = Common::AlignDown(offset, alignment);
            const u32 adjust = offset - offset_aligned;
            ASSERT(adjust % 4 == 0);
            push_data.AddOffset(binding.buffer, adjust);
            buffer_infos.emplace_back(vk_buffer->Handle(), offset_aligned, size + adjust);
            if (auto barrier =
                    vk_buffer->GetBarrier(desc.is_written ? vk::AccessFlagBits2::eShaderWrite
                                                          : vk::AccessFlagBits2::eShaderRead,
                                          vk::PipelineStageFlagBits2::eAllCommands)) {
                buffer_barriers.emplace_back(*barrier);
            }
            if (desc.is_written && desc.is_formatted) {
                texture_cache.InvalidateMemoryFromGPU(vsharp.base_address, size);
            }
        }

        if (capture_draw_buffers &&
            ((stage.stage == Shader::Stage::Vertex && !desc.is_written) ||
             stage.stage == Shader::Stage::Compute) &&
            !desc.IsSpecial() && size && buffer_infos.back().buffer) {
            auto bound = buffer_infos.back();
            // Strip the descriptor alignment prefix; the shader's push offset skips it too.
            bound.offset += bound.range - size;
            bound.range = size;
            draw_buffer_snapshots.push_back({bound, vsharp.base_address,
                fmt::format("sharp{}-stride{}", desc.sharp_idx, vsharp.GetStride())});
        }
        auto& set_write = set_writes[set_write_index++];
        set_write.dstSet = VK_NULL_HANDLE;
        set_write.dstBinding = binding.unified++;
        set_write.dstArrayElement = 0;
        set_write.descriptorCount = 1;
        set_write.descriptorType =
            is_storage ? vk::DescriptorType::eStorageBuffer : vk::DescriptorType::eUniformBuffer;
        set_write.pBufferInfo = &buffer_infos.back();
        ++binding.buffer;
    }
}

bool Rasterizer::BeginComputeBufferCapture(const Shader::Info& stage) {
    capture_draw_buffers = false;
    draw_buffer_snapshots.clear();
    static const u64 selected = [] {
        const auto* value = std::getenv("SHADPS4_DUMP_COMPUTE_BUFFERS");
        return value ? std::strtoull(value, nullptr, 16) : 0ULL;
    }();
    static const auto range = [] {
        std::pair<u64, u64> result{};
        if (const char* value = std::getenv("SHADPS4_DUMP_COMPUTE_RANGE")) {
            char* end = nullptr;
            result.first = std::strtoull(value, &end, 16);
            if (end && *end == ':') result.second = std::strtoull(end + 1, nullptr, 16);
        }
        return result;
    }();
    static const u64 minimum_tick = [] {
        const auto* value = std::getenv("SHADPS4_DUMP_COMPUTE_MIN_TICK");
        return value ? std::strtoull(value, nullptr, 10) : 0ULL;
    }();
    static const bool overlap_only = std::getenv("SHADPS4_DUMP_COMPUTE_OVERLAP_ONLY") != nullptr;
    static const u32 capture_limit = [] {
        const auto* value = std::getenv("SHADPS4_DUMP_COMPUTE_LIMIT");
        return value ? std::clamp(std::strtoul(value, nullptr, 10), 1UL, 64UL) : 12UL;
    }();
    static const u64 minimum_bytes = [] {
        const auto* value = std::getenv("SHADPS4_DUMP_COMPUTE_MIN_BYTES");
        return value ? std::strtoull(value, nullptr, 10) : 0ULL;
    }();
    if (!selected || stage.pgm_hash != selected || compute_buffer_captures >= capture_limit ||
        range.second <= range.first || scheduler.CurrentTick() < minimum_tick) return false;
    for (const auto& desc : stage.buffers) {
        if (!desc.is_written || desc.IsSpecial()) continue;
        const auto sharp = desc.GetSharp(stage);
        const u64 address = sharp.base_address;
        if (!sharp.GetSize() || sharp.GetSize() < minimum_bytes || address >= range.second ||
            (address < range.first && sharp.GetSize() <= range.first - address)) continue;
        if (overlap_only) {
            bool overlap = false;
            for (const auto& source : stage.buffers) {
                if (source.is_written || source.IsSpecial()) continue;
                const auto input = source.GetSharp(stage);
                const u64 begin = input.base_address;
                overlap |= input.GetSize() && begin < address + sharp.GetSize() &&
                           address < begin + input.GetSize();
            }
            if (!overlap) continue;
        }
        capture_draw_buffers = true;
        ++compute_buffer_captures;
        const auto& cs = liverpool->GetCsRegs();
        LOG_INFO(Render_Vulkan, "COMPUTE_BUFFER_CAPTURE capture={} shader={:#x} groups={}x{}x{} user2={} tick={}",
                 captured_draw_count, stage.pgm_hash, cs.dim_x, cs.dim_y, cs.dim_z,
                 cs.user_data[2], scheduler.CurrentTick());
        for (const auto& resource : stage.buffers) {
            if (resource.IsSpecial()) continue;
            const auto buffer = resource.GetSharp(stage);
            LOG_INFO(Render_Vulkan, "COMPUTE_BUFFER_RESOURCE capture={} sharp={} write={} address={:#x} bytes={} stride={}",
                     captured_draw_count, resource.sharp_idx, resource.is_written,
                     static_cast<u64>(buffer.base_address), buffer.GetSize(), buffer.GetStride());
        }
        return true;
    }
    return false;
}

void Rasterizer::DumpBoundDrawBuffers(std::string_view phase) {
    // Diagnostic only: copy the actual bindings after their draw, without touching guest
    // memory or cache dirty flags. Bounded allocations live until this submission completes.
    scheduler.EndRendering();
    const auto cmd = scheduler.CommandBuffer();
    const vk::MemoryBarrier2 before{
        .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands | vk::PipelineStageFlagBits2::eHost,
        .srcAccessMask = vk::AccessFlagBits2::eMemoryWrite | vk::AccessFlagBits2::eHostWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eTransfer,
        .dstAccessMask = vk::AccessFlagBits2::eTransferRead,
    };
    cmd.pipelineBarrier2(vk::DependencyInfo{.memoryBarrierCount = 1, .pMemoryBarriers = &before});
    const auto directory = Common::FS::GetUserPath(Common::FS::PathType::UserDir) / "draw_buffers";
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    if (error) return;
    for (const auto& snapshot : draw_buffer_snapshots) {
        const u64 bytes = snapshot.bound.range;
        // Avoid partial buffers that could be mistaken for complete vertex/index arrays.
        const u64 limit = phase.empty() ? 4 * 1024 * 1024 : 16 * 1024 * 1024;
        if (!bytes || bytes > limit || snapshot.bound.offset % 4 || bytes % 4) {
            LOG_WARNING(Render_Vulkan, "DRAW_BUFFER_CAPTURE skipped {} bytes={}", snapshot.label, bytes);
            continue;
        }
        auto download = std::make_unique<VideoCore::Buffer>(instance, scheduler,
            VideoCore::MemoryUsage::Download, 0, VideoCore::AllFlags,
            Common::AlignUp(bytes, instance.NonCoherentAtomSize()));
        cmd.copyBuffer(snapshot.bound.buffer, download->Handle(),
                       vk::BufferCopy{snapshot.bound.offset, 0, bytes});
        const auto path = directory / fmt::format("draw{:04d}-{}{}-addr{:x}.bin",
            captured_draw_count, phase, snapshot.label, snapshot.address);
        std::vector<u8> cpu_snapshot;
#ifdef _WIN32
        // ReadProcessMemory reports inaccessible pages instead of invoking the emulator's
        // fault handler. This is an unsynchronized CPU observation, not authoritative GPU data.
        cpu_snapshot.resize(bytes);
        SIZE_T read_bytes = 0;
        if (!ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(snapshot.address),
                               cpu_snapshot.data(), bytes, &read_bytes) || read_bytes != bytes) {
            cpu_snapshot.clear();
        }
#endif
        LOG_INFO(Render_Vulkan, "DRAW_BUFFER_SOURCE draw={} label={} address={:#x} bytes={} cpu_snapshot={}",
                 captured_draw_count, snapshot.label, snapshot.address, bytes, !cpu_snapshot.empty());
        scheduler.DeferOperation([download = std::move(download), path, bytes,
                                  cpu_snapshot = std::move(cpu_snapshot),
                                  allocator = instance.GetAllocator()]() mutable {
            if (!download->is_coherent &&
                vmaInvalidateAllocation(allocator, download->buffer.allocation, 0, VK_WHOLE_SIZE) != VK_SUCCESS) {
                LOG_ERROR(Render_Vulkan, "DRAW_BUFFER_CAPTURE invalidate failed");
                return;
            }
            std::ofstream out{path, std::ios::binary};
            out.write(reinterpret_cast<const char*>(download->mapped_data.data()), bytes);
            if (!out) LOG_ERROR(Render_Vulkan, "DRAW_BUFFER_CAPTURE write failed: {}", path.string());
            if (!cpu_snapshot.empty()) {
                auto cpu_path = path;
                cpu_path += ".cpu";
                std::ofstream cpu_out{cpu_path, std::ios::binary};
                cpu_out.write(reinterpret_cast<const char*>(cpu_snapshot.data()), bytes);
                LOG_INFO(Render_Vulkan, "DRAW_BUFFER_COMPARE file={} equal={}", path.filename().string(),
                         std::memcmp(cpu_snapshot.data(), download->mapped_data.data(), bytes) == 0);
            }
        });
    }
    // Order subsequent writers after our reads and make download writes host-visible.
    const vk::MemoryBarrier2 after{
        .srcStageMask = vk::PipelineStageFlagBits2::eTransfer,
        .srcAccessMask = vk::AccessFlagBits2::eTransferRead | vk::AccessFlagBits2::eTransferWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eAllCommands | vk::PipelineStageFlagBits2::eHost,
        .dstAccessMask = vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite | vk::AccessFlagBits2::eHostRead,
    };
    cmd.pipelineBarrier2(vk::DependencyInfo{.memoryBarrierCount = 1, .pMemoryBarriers = &after});
}

std::unique_ptr<VideoCore::Buffer> Rasterizer::IsolateReadConstGuestBuffer(VAddr addr, u64 size) {
    const u64 alloc_size = std::max<u64>(size, instance.NonCoherentAtomSize());
    auto isolated = std::make_unique<VideoCore::Buffer>(instance, scheduler, VideoCore::MemoryUsage::Upload,
                                                        addr, VideoCore::AllFlags, alloc_size);
    ASSERT_MSG(!isolated->mapped_data.empty(),
               "ReadConst snapshot buffer is not host-visible");
    const auto* src = reinterpret_cast<const u8*>(addr);
    std::memcpy(isolated->mapped_data.data(), src, size);
    if (size < alloc_size) {
        std::memset(isolated->mapped_data.data() + size, 0, alloc_size - size);
    }
    if (!isolated->is_coherent) {
        const VkDeviceSize flush_size =
            Common::AlignUp(alloc_size, instance.NonCoherentAtomSize());
        const VkResult flush_result =
            vmaFlushAllocation(instance.GetAllocator(), isolated->buffer.allocation, 0, flush_size);
        ASSERT_MSG(flush_result == VK_SUCCESS, "vmaFlushAllocation failed: {}",
                   vk::to_string(vk::Result{flush_result}));
    }

    ++isolated_readconst_hits;
    if (isolated_readconst_hits <= 16 || (isolated_readconst_hits % 120) == 0) {
        u32 checksum = 0;
        const u32 dword_count = static_cast<u32>(size / 4);
        const auto* dwords = reinterpret_cast<const u32*>(src);
        for (u32 i = 0; i < dword_count; ++i) {
            checksum ^= dwords[i] + i * 0x9e3779b9u;
        }
        LOG_WARNING(Render_Vulkan,
                    "READCONST_SNAPSHOT hit={} addr={:#x} size={:#x} coherent={} checksum={:#010x}",
                    isolated_readconst_hits, addr, size, isolated->is_coherent, checksum);
        const ReadConstFloatStats float_stats =
            ClassifyReadConstFloats(std::span{dwords, dword_count});
        std::array<float, 8> head{};
        for (u32 i = 0; i < std::min<u32>(head.size(), dword_count); ++i) {
            head[i] = std::bit_cast<float>(dwords[i]);
        }
        // DSR's skinned VS divides the runtime vertex index by a per-frame
        // divisor read from const dwords 152..155 (rcp-based integer division
        // emulation feeding the x80-stride fetch address). Export them raw so
        // idle-vs-walk divisor values can be compared offline.
        std::array<u32, 5> div{};
        const bool have_div = dword_count >= 157;
        if (have_div) {
            std::memcpy(div.data(), dwords + 152, div.size() * sizeof(u32));
        }
        LOG_WARNING(Render_Vulkan,
                    "READCONST_FLOATS hit={} floats={} nan={} inf={} denorm={} huge1e4={} "
                    "maxabs={:.6g} head=[{:.4g},{:.4g},{:.4g},{:.4g},{:.4g},{:.4g},{:.4g},{:.4g}] "
                    "div152=[{:#010x},{:#010x},{:#010x},{:#010x},{:#010x}]",
                    isolated_readconst_hits, dword_count, float_stats.nan_count,
                    float_stats.inf_count, float_stats.denorm_count, float_stats.huge_count,
                    float_stats.max_abs, head[0], head[1], head[2], head[3], head[4], head[5],
                    head[6], head[7], div[0], div[1], div[2], div[3], div[4]);
    }
    return isolated;
}

void Rasterizer::RetireIsolatedReadConstSnapshots() {
    if (isolated_readconst_buffers.empty()) {
        return;
    }
    scheduler.Finish();
    isolated_readconst_buffers.clear();
}

void Rasterizer::BindTextures(const Shader::Info& stage, Shader::Backend::Bindings& binding) {
    image_bindings.clear();
    const u32 first_image_idx = image_infos.size();
    // For loading/storing to explicit mip levels, when no native instruction support, bind an array
    // of descriptors consecutively, 1 for each mip level. The shader can index this with LOD
    // operand.
    // This array holds the size of each consecutive array with the number of bindings consumed.
    // This is currently always 1 for anything other than mip fallback arrays.
    boost::container::small_vector<u32, 8> image_descriptor_array_sizes;

    for (const auto& image_desc : stage.images) {
        const auto tsharp = image_desc.GetSharp(stage);
        if (texture_cache.IsMeta(tsharp.Address())) {
            LOG_WARNING(Render_Vulkan, "Unexpected metadata read by a shader (texture)");
            if (std::getenv("SHADPS4_TRACE_HMD_LAYOUT")) {
                static std::unordered_set<std::string> reported;
                const auto key = fmt::format("{}:{}", stage.pgm_hash, tsharp.Address());
                if (reported.size() < 64 && reported.insert(key).second) {
                    LOG_WARNING(Render_Vulkan,
                                "META_TEXTURE shader={:#x} stage={} addr={:#x} htile={} "
                                "type={} size={}x{}x{} fmt={}/{} written={}",
                                stage.pgm_hash, static_cast<u32>(stage.stage), tsharp.Address(),
                                texture_cache.IsHtile(tsharp.Address()),
                                static_cast<u32>(tsharp.GetType()), tsharp.width + 1,
                                tsharp.height + 1, tsharp.depth + 1,
                                static_cast<u32>(tsharp.GetDataFmt()),
                                static_cast<u32>(tsharp.GetNumberFmt()), image_desc.is_written);
                }
            }
        }

        if (tsharp.GetDataFmt() == AmdGpu::DataFormat::FormatInvalid) {
            image_bindings.emplace_back(std::piecewise_construct, std::tuple{}, std::tuple{});
            image_descriptor_array_sizes.push_back(1);
            continue;
        }

        const Shader::MipStorageFallbackMode mip_fallback_mode = image_desc.mip_fallback_mode;
        const u32 num_bindings = image_desc.NumBindings(stage);

        for (auto i = 0; i < num_bindings; i++) {
            auto& [image_id, desc] = image_bindings.emplace_back(
                std::piecewise_construct, std::tuple{}, std::tuple{tsharp, image_desc});

            if (mip_fallback_mode == Shader::MipStorageFallbackMode::ConstantIndex) {
                ASSERT(num_bindings == 1);
                desc.view_info.range.base.level += image_desc.constant_mip_index;
                desc.view_info.range.extent.levels = 1;
            } else if (mip_fallback_mode == Shader::MipStorageFallbackMode::DynamicIndex) {
                desc.view_info.range.base.level += i;
                desc.view_info.range.extent.levels = 1;
            }

            image_id = texture_cache.FindImage(desc);
            auto* image = &texture_cache.GetImage(image_id);
            if (auto depth_image_id = texture_cache.GetAssociatedDepth(*image)) {
                // The allocation may have been recycled since its stencil association
                // was recorded. Only stencil-compatible samples can use that alias;
                // redirecting an RGBA texture would create an invalid depth image view.
                if (LiverpoolToVK::IsFormatStencilCompatible(desc.view_info.format)) {
                    image_id = depth_image_id;
                    image = &texture_cache.GetImage(image_id);
                } else {
                    static u32 reports = 0;
                    if (reports++ < 16) {
                        const auto& depth = texture_cache.GetImage(depth_image_id);
                        LOG_INFO(Render_Vulkan,
                                 "STALE_STENCIL_ALIAS_REJECTED texture={:#x} format={} depth={:#x} shader={:#x}",
                                 desc.info.guest_address, vk::to_string(desc.view_info.format),
                                 depth.info.guest_address, stage.pgm_hash);
                    }
                }
            }
            if (image->binding.is_bound) {
                // The image is already bound. In case if it is about to be used as storage we
                // need to force general layout on it.
                image->binding.force_general |= image_desc.is_written;
            }
            image->binding.is_bound = 1u;
        }

        image_descriptor_array_sizes.push_back(num_bindings);
    }

    // While draws are traced: what each shader samples, once a shader, and what the texture
    // cache found for it.
    if (g_draw_trace_budget.load(std::memory_order_relaxed) > 0) {
        static std::mutex traced_mutex;
        static std::unordered_set<u64> traced_shaders;
        bool first;
        {
            std::scoped_lock lock{traced_mutex};
            first = traced_shaders.insert(stage.pgm_hash).second;
        }
        if (first) {
            u32 index = 0;
            for (const auto& image_desc : stage.images) {
                const auto tsharp = image_desc.GetSharp(stage);
                const auto& [image_id, desc] = image_bindings[std::min<size_t>(
                    index, image_bindings.size() - 1)];
                std::string found = "nothing";
                if (image_id) {
                    const auto& image = texture_cache.GetImage(image_id);
                    found = fmt::format(
                        "{}x{}x{} {} at {:#x} levels {} layers {} size {:#x}{}{} (asked: levels {} "
                        "layers {} size {:#x})",
                        image.info.size.width, image.info.size.height, image.info.size.depth,
                        vk::to_string(image.info.pixel_format), image.info.guest_address,
                        image.info.resources.levels, image.info.resources.layers,
                        image.info.guest_size, image.info.props.is_depth ? " depth" : "",
                        image.binding.is_target ? " target" : "", desc.info.resources.levels,
                        desc.info.resources.layers, desc.info.guest_size);
                }
                LOG_INFO(Render_Vulkan,
                         "TRACE   texture {} of shader {:#x} (stage {}, cb0 {:#x}, {} indices): "
                         "type {} {}x{}x{} fmt {}/{} levels "
                         "{}-{} at {:#x}{} -> {} min_lod={} pitch={} tile={} alt={}",
                         index, stage.pgm_hash, static_cast<u32>(stage.stage),
                         liverpool->regs.color_buffers[0].Address(), liverpool->regs.num_indices,
                         static_cast<u32>(tsharp.GetType()),
                         tsharp.width + 1, tsharp.height + 1, tsharp.depth + 1,
                         static_cast<u32>(tsharp.GetDataFmt()),
                         static_cast<u32>(tsharp.GetNumberFmt()), tsharp.base_level,
                         tsharp.last_level, tsharp.Address(),
                         image_desc.is_written ? " written" : "", found,
                         static_cast<float>(tsharp.min_lod) / 256.0f, tsharp.Pitch(),
                         static_cast<u32>(tsharp.GetTileMode()), bool(tsharp.alt_tile_mode));
                index += image_desc.NumBindings(stage);
            }
        }
    }

    // Second pass to re-bind images that were updated after binding
    for (auto& [image_id, desc] : image_bindings) {
        bool is_storage = desc.type == VideoCore::TextureCache::BindingType::Storage;
        if (!image_id) {
            if (instance.IsNullDescriptorSupported()) {
                image_infos.emplace_back(VK_NULL_HANDLE, VK_NULL_HANDLE, vk::ImageLayout::eGeneral);
            } else {
                auto& null_image_view = texture_cache.FindTexture(VideoCore::NULL_IMAGE_ID, desc);
                image_infos.emplace_back(VK_NULL_HANDLE, *null_image_view.image_view,
                                         vk::ImageLayout::eGeneral);
            }
        } else {
            if (auto& old_image = texture_cache.GetImage(image_id);
                old_image.binding.needs_rebind) {
                old_image.binding = {};
                image_id = texture_cache.FindImage(desc);
            }

            bound_images.emplace_back(image_id);

            auto& image = texture_cache.GetImage(image_id);
            auto& image_view = texture_cache.FindTexture(image_id, desc);

            // The image is either bound as storage in a separate descriptor or bound as render
            // target in feedback loop. Depth images are excluded because they can't be bound as
            // storage and feedback loop doesn't make sense for them
            if ((image.binding.force_general || image.binding.is_target) &&
                !image.info.props.is_depth) {
                image.Transit(instance.IsAttachmentFeedbackLoopLayoutSupported() &&
                                      image.binding.is_target
                                  ? vk::ImageLayout::eAttachmentFeedbackLoopOptimalEXT
                                  : vk::ImageLayout::eGeneral,
                              vk::AccessFlagBits2::eShaderRead |
                                  (image.info.props.is_depth
                                       ? vk::AccessFlagBits2::eDepthStencilAttachmentWrite
                                       : vk::AccessFlagBits2::eColorAttachmentWrite |
                                             vk::AccessFlagBits2::eColorAttachmentRead),
                              {});
            } else {
                if (is_storage) {
                    image.Transit(vk::ImageLayout::eGeneral,
                                  vk::AccessFlagBits2::eShaderRead |
                                      vk::AccessFlagBits2::eShaderWrite,
                                  desc.view_info.range);
                } else if (image.info.props.is_depth && image.binding.is_target) {
                    // The draw tests against the depth buffer it reads: one state for both
                    // uses, so that a run of such draws does not go back and forth between
                    // two (which would end the render pass before each of them).
                    const auto state = GetDepthTargetState(
                        image, liverpool->regs, instance.IsAttachmentFeedbackLoopLayoutSupported());
                    image.Transit(state.layout, state.access, desc.view_info.range);
                } else {
                    const auto new_layout = image.info.props.is_depth
                                                ? vk::ImageLayout::eDepthStencilReadOnlyOptimal
                                                : vk::ImageLayout::eShaderReadOnlyOptimal;
                    image.Transit(new_layout, vk::AccessFlagBits2::eShaderRead,
                                  desc.view_info.range);
                }
            }
            image.usage.storage |= is_storage;
            image.usage.texture |= !is_storage;

            image_infos.emplace_back(VK_NULL_HANDLE, *image_view.image_view,
                                     image.backing->state.layout);
        }
    }

    u32 image_info_idx = first_image_idx;
    u32 image_binding_idx = 0;
    for (u32 array_size : image_descriptor_array_sizes) {
        const auto& [_, desc] = image_bindings[image_binding_idx];
        const bool is_storage = desc.type == VideoCore::TextureCache::BindingType::Storage;
        auto& set_write = set_writes[set_write_index++];
        set_write.dstSet = VK_NULL_HANDLE;
        set_write.dstBinding = binding.unified;
        set_write.dstArrayElement = 0;
        set_write.descriptorCount = array_size;
        set_write.descriptorType =
            is_storage ? vk::DescriptorType::eStorageImage : vk::DescriptorType::eSampledImage;
        set_write.pImageInfo = &image_infos[image_info_idx];

        image_info_idx += array_size;
        image_binding_idx += array_size;
        binding.unified += array_size;
    }

    for (const auto& sampler : stage.samplers) {
        auto ssharp = sampler.GetSharp(stage);
        if (sampler.disable_aniso) {
            const auto& tsharp = stage.images[sampler.associated_image].GetSharp(stage);
            if (tsharp.base_level == 0 && tsharp.last_level == 0) {
                ssharp.max_aniso.Assign(AmdGpu::AnisoRatio::One);
            }
        }
        const auto vk_sampler = texture_cache.GetSampler(ssharp, liverpool->regs.ta_bc_base);
        image_infos.emplace_back(vk_sampler, VK_NULL_HANDLE, vk::ImageLayout::eGeneral);
        auto& set_write = set_writes[set_write_index++];
        set_write.dstSet = VK_NULL_HANDLE;
        set_write.dstBinding = binding.unified++;
        set_write.dstArrayElement = 0;
        set_write.descriptorCount = 1;
        set_write.descriptorType = vk::DescriptorType::eSampler;
        set_write.pImageInfo = &image_infos.back();
    }
}

RenderState Rasterizer::BeginRendering(const GraphicsPipeline* pipeline) {
    attachment_feedback_loop = {};
    const auto& regs = liverpool->regs;
    const auto& key = pipeline->GetGraphicsKey();
    // A depth-only target leaves the stencil flags untouched below (and vice
    // versa). Start with both aspects absent, rather than reading stack bytes
    // as bools and possibly passing an invalid stencil attachment to Vulkan.
    RenderState state{};
    state.width = instance.GetMaxFramebufferWidth();
    state.height = instance.GetMaxFramebufferHeight();
    state.num_layers = std::numeric_limits<u16>::max();
    state.num_color_attachments = std::bit_width(key.mrt_mask);
    for (auto cb = 0u; cb < state.num_color_attachments; ++cb) {
        auto& [image_id, desc] = cb_descs[cb];
        if (!image_id) {
            state.color_attachments[cb] = {};
            continue;
        }
        auto* image = &texture_cache.GetImage(image_id);
        if (image->binding.needs_rebind) {
            image_id = bound_images.emplace_back(texture_cache.FindImage(desc));
            image = &texture_cache.GetImage(image_id);
        }
        texture_cache.UpdateImage(image_id);
        image->SetBackingSamples(key.color_samples[cb]);
        const auto& image_view = texture_cache.FindRenderTarget(image_id, desc);
        const auto slice = image_view.info.range.base.layer;
        const auto mip = image_view.info.range.base.level;

        const auto& col_buf = regs.color_buffers[cb];
        const bool is_clear = texture_cache.IsMetaCleared(col_buf.CmaskAddress(), slice);
        texture_cache.TouchMeta(col_buf.CmaskAddress(), slice, false);

        if (image->binding.is_bound) {
            ASSERT_MSG(!image->binding.force_general,
                       "Having image both as storage and render target is unsupported");
            image->Transit(instance.IsAttachmentFeedbackLoopLayoutSupported()
                               ? vk::ImageLayout::eAttachmentFeedbackLoopOptimalEXT
                               : vk::ImageLayout::eGeneral,
                           vk::AccessFlagBits2::eColorAttachmentWrite, {});
            attachment_feedback_loop |= vk::ImageAspectFlagBits::eColor;
        } else {
            image->Transit(vk::ImageLayout::eColorAttachmentOptimal,
                           vk::AccessFlagBits2::eColorAttachmentWrite |
                               vk::AccessFlagBits2::eColorAttachmentRead,
                           desc.view_info.range);
        }

        state.width = std::min<u32>(state.width, std::max(image->info.size.width >> mip, 1u));
        state.height = std::min<u32>(state.height, std::max(image->info.size.height >> mip, 1u));
        state.num_layers = std::min<u32>(state.num_layers, image_view.info.range.extent.layers);

        const auto clear_value =
            is_clear ? LiverpoolToVK::ColorBufferClearValue(col_buf) : vk::ClearValue{};
        auto& attachment = state.color_attachments[cb];
        attachment.image_view = *image_view.image_view;
        attachment.image_layout = image->backing->state.layout;
        attachment.clear_value = clear_value.color.uint32;
        attachment.is_clear = is_clear;

        image->usage.render_target = 1u;
    }
    for (u32 cb = state.num_color_attachments; cb < state.color_attachments.size(); ++cb) {
        state.color_attachments[cb] = {};
    }

    if (auto image_id = db_desc.first; image_id) {
        auto& desc = db_desc.second;
        const auto htile_address = regs.depth_htile_data_base.GetAddress();
        const auto& image_view = texture_cache.FindDepthTarget(image_id, desc);
        auto& image = texture_cache.GetImage(image_id);

        const auto slice = image_view.info.range.base.layer;
        const bool is_depth_clear = regs.depth_render_control.depth_clear_enable ||
                                    texture_cache.IsMetaCleared(htile_address, slice);
        const bool is_stencil_clear = regs.depth_render_control.stencil_clear_enable &&
                                      !StatikStencilClearDraw(regs) &&
                                      !TryExpandRushStencilMask(image);
        static const bool trace_stencil = std::getenv("SHADPS4_TRACE_STENCIL_REGS") != nullptr;
        if (trace_stencil && htile_address &&
            regs.depth_buffer.StencilValid() && memory->ClampRangeSize(htile_address, 4100) == 4100) {
            static std::unordered_set<u64> seen;
            const u64 key = htile_address ^ (u64{regs.reg_array[0xa2b0]} << 32) ^
                            (u64{is_stencil_clear} << 63);
            if (seen.size() < 64 && seen.insert(key).second) {
                const auto* words = reinterpret_cast<const u32*>(htile_address);
                LOG_INFO(Render_Vulkan,
                         "HTILE_STENCIL addr={:#x} depth={:#x} stencil={:#x} surface={:#x} "
                         "sresults={:#x}/{:#x} override={:#x} clear={}/{} words={:#x}/{:#x}/{:#x}/{:#x}",
                         htile_address, regs.depth_buffer.DepthAddress(),
                         regs.depth_buffer.StencilAddress(), regs.reg_array[0xa2af],
                         regs.reg_array[0xa2b0], regs.reg_array[0xa2b1],
                         std::bit_cast<u32>(regs.depth_render_override), is_depth_clear,
                         is_stencil_clear, words[0], words[1], words[128], words[1024]);
            }
        }
        texture_cache.TouchMeta(htile_address, slice, false);
        ASSERT(desc.view_info.range.extent.levels == 1 && !image.binding.needs_rebind);

        const auto target_state = GetDepthTargetState(
            image, regs, instance.IsAttachmentFeedbackLoopLayoutSupported());
        if (image.binding.is_bound && regs.depth_control.depth_write_enable) {
            attachment_feedback_loop |= vk::ImageAspectFlagBits::eDepth;
        }
        image.Transit(target_state.layout, target_state.access, desc.view_info.range);

        state.width = std::min<u32>(state.width, image.info.size.width);
        state.height = std::min<u32>(state.height, image.info.size.height);
        state.num_layers = std::min<u32>(state.num_layers, image_view.info.range.extent.layers);

        auto& attachment = state.depth_stencil_attachment;
        attachment.image_view = *image_view.image_view;
        attachment.image_layout = image.backing->state.layout;
        attachment.clear_value = {};

        if (regs.depth_buffer.DepthValid()) {
            attachment.clear_value[0] = is_depth_clear ? std::bit_cast<u32>(regs.depth_clear) : 0u;
            attachment.has_depth = true;
            attachment.depth_clear = is_depth_clear;
        }
        if (regs.depth_buffer.StencilValid()) {
            attachment.clear_value[1] = is_stencil_clear ? regs.stencil_clear : 0u;
            attachment.has_stencil = true;
            attachment.stencil_clear = is_stencil_clear;
        }

        image.usage.depth_target = true;
    } else {
        state.depth_stencil_attachment = {};
    }

    if (state.num_layers == std::numeric_limits<u16>::max()) {
        state.num_layers = 1;
    }

    return state;
}

bool Rasterizer::TryExpandRushStencilMask(VideoCore::Image& image) {
    const auto percent = Core::KnownTitle::RushResolution::active_percent.load();
    const auto aligned = Core::KnownTitle::RushResolution::active_alignment.load();
    const auto scaled = [&](u32 value) {
        return Core::KnownTitle::RushResolution::Scaled(value, percent, aligned);
    };
    const bool neo_mask = EmulatorSettings.IsNeo() &&
                          image.info.size.width == scaled(2688) && image.info.size.height == scaled(1512);
    const bool expected_size =
        (image.info.size.width == scaled(2304) && image.info.size.height == scaled(1296)) ||
        neo_mask;
    static const bool enabled = [] {
        const char* value = std::getenv("SHADPS4_TEST_RUSH_HTILE_STENCIL");
        return value && std::string_view{value} == "1";
    }();
    const auto& regs = liverpool->regs;
    // A title-scoped bridge for its CPU-prepared HiS visibility template. Do not
    // reinterpret arbitrary GPU-generated metadata or change stencil comparisons.
    if (!enabled || Common::ElfInfo::Instance().GameSerial() != "CUSA02350" ||
        !regs.depth_buffer.StencilValid() || !regs.depth_buffer.z_info.tile_surface_enable ||
        regs.reg_array[0xa2af] != 8 || regs.reg_array[0xa2b0] != 0x010ff103 ||
        regs.depth_control.stencil_ref_func != AmdGpu::CompareFunc::LessEqual ||
        regs.depth_control.backface_enable || regs.stencil_ref_front.stencil_test_val != 16 ||
        regs.stencil_ref_front.stencil_mask != 255 || std::bit_cast<u32>(regs.stencil_control) != 0 ||
        image.info.num_samples != 1 || image.info.resources.layers != 1 ||
        !expected_size ||
        AmdGpu::GetPipeConfig(image.info.tile_mode) != AmdGpu::PipeConfig::P8_32x32_16x16) {
        return false;
    }
    const VAddr address = regs.depth_htile_data_base.GetAddress();
    // Rush's CPU Neo writer uses its own P16 permutation (descriptor field 0x12).
    // The DB tile-mode index alone still describes the base-console P8 layout.
    const auto layout = neo_mask ? AmdGpu::HtileStencil::Layout::RushNeoP16
                                 : AmdGpu::HtileStencil::Layout::P8_32x32_16x16;
    const u32 word_count =
        AmdGpu::HtileStencil::WordCount(image.info.pitch, image.info.size.height, layout);
    const u64 byte_count = u64{word_count} * sizeof(u32);
    if (!address || memory->ClampRangeSize(address, byte_count) != byte_count) {
        rush_hidden_rects.clear();
        rush_mask_size = {};
        return false;
    }
    static thread_local std::vector<u8> stencil;
    if (!AmdGpu::HtileStencil::DecodeRushMask(
            {reinterpret_cast<const u32*>(address), word_count}, image.info.size.width,
            image.info.size.height, image.info.pitch, stencil, layout)) {
        rush_hidden_rects.clear();
        rush_mask_size = {};
        return false;
    }
    static const bool export_mask = [] {
        const auto* value = std::getenv("SHADPS4_TEST_RUSH_COMPOSITOR_MASK");
        return value && std::string_view{value} == "1";
    }();
    if (export_mask) {
        rush_hidden_rects = AmdGpu::HtileStencil::HiddenRects(
            stencil, image.info.size.width, image.info.size.height);
        rush_mask_size = {image.info.size.width, image.info.size.height};
    }
    scheduler.EndRendering();
    auto& stream = buffer_cache.GetUtilityBuffer(VideoCore::MemoryUsage::Stream);
    const u64 offset = stream.Copy(stencil.data(), stencil.size(), 4);
    image.Transit(vk::ImageLayout::eTransferDstOptimal, vk::AccessFlagBits2::eTransferWrite, {});
    const vk::BufferMemoryBarrier2 barrier{
        .srcStageMask = vk::PipelineStageFlagBits2::eHost,
        .srcAccessMask = vk::AccessFlagBits2::eHostWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eTransfer,
        .dstAccessMask = vk::AccessFlagBits2::eTransferRead,
        .buffer = stream.Handle(),
        .offset = offset,
        .size = stencil.size(),
    };
    const auto cmdbuf = scheduler.CommandBuffer();
    cmdbuf.pipelineBarrier2(vk::DependencyInfo{
        .bufferMemoryBarrierCount = 1, .pBufferMemoryBarriers = &barrier});
    const vk::BufferImageCopy region{
        .bufferOffset = offset,
        .imageSubresource{.aspectMask = vk::ImageAspectFlagBits::eStencil, .layerCount = 1},
        .imageExtent = {image.info.size.width, image.info.size.height, 1},
    };
    cmdbuf.copyBufferToImage(stream.Handle(), image.GetImage(),
                            vk::ImageLayout::eTransferDstOptimal, region);
    static u32 reports{};
    if (reports++ < 4) {
        LOG_INFO(Render_Vulkan, "RUSH_HTILE_STENCIL expanded {}x{} HiS visibility mask, {} layout; stencil test retained",
                 image.info.size.width, image.info.size.height, neo_mask ? "P16" : "P8");
    }
    return true;
}

void Rasterizer::Resolve() {
    const auto& mrt0_hint = liverpool->last_cb_extent[0];
    const auto& mrt1_hint = liverpool->last_cb_extent[1];
    VideoCore::TextureCache::ImageDesc mrt0_desc{liverpool->regs.color_buffers[0], mrt0_hint};
    VideoCore::TextureCache::ImageDesc mrt1_desc{liverpool->regs.color_buffers[1], mrt1_hint};
    auto& mrt0_image = texture_cache.GetImage(texture_cache.FindImage(mrt0_desc, true));
    auto& mrt1_image = texture_cache.GetImage(texture_cache.FindImage(mrt1_desc, true));

    ScopeMarkerBegin(fmt::format("Resolve:MRT0={:#x}:MRT1={:#x}",
                                 liverpool->regs.color_buffers[0].Address(),
                                 liverpool->regs.color_buffers[1].Address()));
    mrt1_image.Resolve(mrt0_image, mrt0_desc.view_info.range, mrt1_desc.view_info.range);
    ScopeMarkerEnd();
}

void Rasterizer::DepthStencilCopy(bool is_depth, bool is_stencil) {
    auto& regs = liverpool->regs;

    auto read_desc = VideoCore::TextureCache::ImageDesc(
        regs.depth_buffer, regs.depth_view, regs.depth_control,
        regs.depth_htile_data_base.GetAddress(), liverpool->last_db_extent, false);
    auto write_desc = VideoCore::TextureCache::ImageDesc(
        regs.depth_buffer, regs.depth_view, regs.depth_control,
        regs.depth_htile_data_base.GetAddress(), liverpool->last_db_extent, true);

    const auto read_id = texture_cache.FindImage(read_desc);
    const auto write_id = texture_cache.FindImage(write_desc);
    texture_cache.AssociateStencil(write_id, write_desc.info);
    auto& read_image = texture_cache.GetImage(read_id);
    auto& write_image = texture_cache.GetImage(write_id);

    if (std::getenv("SHADPS4_TRACE_STENCIL_COPY")) {
        LOG_INFO(Render_Vulkan,
                 "STENCIL_COPY source_depth={:#x} source_stencil={:#x} dest_depth={:#x} "
                 "dest_stencil={:#x} depth={} stencil={} layers={} bytes={}",
                 read_desc.info.guest_address, read_desc.info.stencil_addr,
                 write_desc.info.guest_address, write_desc.info.stencil_addr,
                 is_depth, is_stencil, write_desc.info.resources.layers,
                 write_desc.info.stencil_size);
    }

    VideoCore::SubresourceRange sub_range;
    sub_range.base.layer = liverpool->regs.depth_view.slice_start;
    sub_range.extent.layers = liverpool->regs.depth_view.NumSlices() - sub_range.base.layer;

    ScopeMarkerBegin(fmt::format(
        "DepthStencilCopy:DR={:#x}:SR={:#x}:DW={:#x}:SW={:#x}", regs.depth_buffer.DepthAddress(),
        regs.depth_buffer.StencilAddress(), regs.depth_buffer.DepthWriteAddress(),
        regs.depth_buffer.StencilWriteAddress()));

    read_image.Transit(vk::ImageLayout::eTransferSrcOptimal, vk::AccessFlagBits2::eTransferRead,
                       sub_range);
    write_image.Transit(vk::ImageLayout::eTransferDstOptimal, vk::AccessFlagBits2::eTransferWrite,
                        sub_range);

    auto aspect_mask = vk::ImageAspectFlags(0);
    if (is_depth) {
        aspect_mask |= vk::ImageAspectFlagBits::eDepth;
    }
    if (is_stencil) {
        aspect_mask |= vk::ImageAspectFlagBits::eStencil;
    }

    vk::ImageCopy region = {
        .srcSubresource =
            {
                .aspectMask = aspect_mask,
                .mipLevel = 0,
                .baseArrayLayer = sub_range.base.layer,
                .layerCount = sub_range.extent.layers,
            },
        .srcOffset = {0, 0, 0},
        .dstSubresource =
            {
                .aspectMask = aspect_mask,
                .mipLevel = 0,
                .baseArrayLayer = sub_range.base.layer,
                .layerCount = sub_range.extent.layers,
            },
        .dstOffset = {0, 0, 0},
        .extent = {write_image.info.size.width, write_image.info.size.height, 1},
    };
    scheduler.CommandBuffer().copyImage(read_image.GetImage(), vk::ImageLayout::eTransferSrcOptimal,
                                        write_image.GetImage(),
                                        vk::ImageLayout::eTransferDstOptimal, region);

    ScopeMarkerEnd();
}

bool Rasterizer::TryHtileClear(VAddr address, std::span<const u32> htile_words) {
    if (htile_words.empty() || !texture_cache.IsHtile(address)) {
        return false;
    }

    // The low four bits of an HTILE entry are its zmask; zero means every pixel of that 8x8 tile
    // holds the depth clear value. Titles clear depth by filling the surface with such entries,
    // or by copying in a template they prepared once. A template may also mark some tiles as
    // permanently occluded (zero depth range), which is how VR titles skip the part of the eye
    // buffer the lenses cannot show; those tiles only save work, so a plain clear is equivalent.
    u64 cleared = 0;
    u64 written = 0;
    for (const u32 word : htile_words) {
        if (word == 0) {
            // Padding past the end of the surface.
            continue;
        }
        ++written;
        cleared += (word & 0xf) == 0;
    }
    if (cleared * 2 < written || cleared == 0) {
        return false;
    }
    return texture_cache.ClearMeta(address);
}

void Rasterizer::FillBuffer(VAddr address, u32 num_bytes, u32 value, bool is_gds) {
    if (TraceDraw()) {
        LOG_INFO(Render_Vulkan, "TRACE fill addr={:#x} bytes={} value={:#x} gds={}",
                 address, num_bytes, value, is_gds);
    }
    if (TraceDraw()) {
        LOG_INFO(Render_Vulkan, "TRACE dma-fill dst={:#x} bytes={:#x} value={:#x} gds={} is_meta={}",
                 address, num_bytes, value, is_gds, texture_cache.IsMeta(address));
    }
    if (!is_gds && value != 0 && TryHtileClear(address, std::span{&value, 1})) {
        return;
    }
    buffer_cache.FillBuffer(address, num_bytes, value, is_gds);
}

void Rasterizer::CopyBuffer(VAddr dst, VAddr src, u32 num_bytes, bool dst_gds, bool src_gds) {
    if (TraceDraw()) {
        LOG_INFO(Render_Vulkan, "TRACE copy src={:#x} dst={:#x} bytes={} gds={}/{}",
                 src, dst, num_bytes, src_gds, dst_gds);
    }
    if (TraceDraw()) {
        LOG_INFO(Render_Vulkan, "TRACE dma-copy dst={:#x} src={:#x} bytes={:#x} is_meta={}", dst,
                 src, num_bytes, texture_cache.IsMeta(dst));
    }
    if (!dst_gds && !src_gds &&
        TryHtileClear(dst, std::span{reinterpret_cast<const u32*>(src), num_bytes / sizeof(u32)})) {
        return;
    }
    buffer_cache.CopyBuffer(dst, src, num_bytes, dst_gds, src_gds);
}

u32 Rasterizer::ReadDataFromGds(u32 gds_offset) {
    auto* gds_buf = buffer_cache.GetGdsBuffer();
    u32 value;
    std::memcpy(&value, gds_buf->mapped_data.data() + gds_offset, sizeof(u32));
    return value;
}

bool Rasterizer::InvalidateMemory(VAddr addr, u64 size) {
    if (!IsMapped(addr, size)) {
        // Not GPU mapped memory, can skip invalidation logic entirely.
        return false;
    }
    buffer_cache.InvalidateMemory(addr, size);
    texture_cache.InvalidateMemory(addr, size);
    return true;
}

bool Rasterizer::ReadMemory(VAddr addr, u64 size) {
    if (!IsMapped(addr, size)) {
        // Not GPU mapped memory, can skip invalidation logic entirely.
        return false;
    }
    buffer_cache.ReadMemory(addr, size);
    return true;
}

bool Rasterizer::IsMapped(VAddr addr, u64 size) {
    if (size == 0) {
        // There is no memory, so not mapped.
        return false;
    }
    if (static_cast<u64>(addr) > std::numeric_limits<u64>::max() - size) {
        // Memory range wrapped the address space, cannot be mapped.
        return false;
    }
    const auto range = decltype(mapped_ranges)::interval_type::right_open(addr, addr + size);

    Common::RecursiveSharedLock lock{mapped_ranges_mutex};
    return boost::icl::contains(mapped_ranges, range);
}

void Rasterizer::MapMemory(VAddr addr, u64 size) {
    if (TraceBufferWrite(addr, size)) {
        LOG_INFO(Render_Vulkan, "BUFFER_GPU_MAP address={:#x} bytes={}", addr, size);
    }
    {
        std::scoped_lock lock{mapped_ranges_mutex};
        mapped_ranges += decltype(mapped_ranges)::interval_type::right_open(addr, addr + size);
    }
    page_manager.OnGpuMap(addr, size);
}

void Rasterizer::UnmapMemory(VAddr addr, u64 size) {
    if (TraceBufferWrite(addr, size)) {
        LOG_INFO(Render_Vulkan, "BUFFER_GPU_UNMAP address={:#x} bytes={}", addr, size);
    }
    buffer_cache.InvalidateMemory(addr, size);
    texture_cache.UnmapMemory(addr, size);
    page_manager.OnGpuUnmap(addr, size);
    {
        std::scoped_lock lock{mapped_ranges_mutex};
        mapped_ranges -= decltype(mapped_ranges)::interval_type::right_open(addr, addr + size);
    }
}

void Rasterizer::UpdateDynamicState(const GraphicsPipeline* pipeline, const bool is_indexed) const {
    UpdateViewportScissorState();
    UpdateDepthStencilState();
    UpdatePrimitiveState(is_indexed);
    UpdateRasterizationState();
    UpdateColorBlendingState(pipeline);

    auto& dynamic_state = scheduler.GetDynamicState();
    dynamic_state.Commit(instance, scheduler.CommandBuffer());
}

void Rasterizer::UpdateViewportScissorState() const {
    const auto& regs = liverpool->regs;

    const auto combined_scissor_value_tl = [](s16 scr, s16 win, s16 gen, s16 win_offset) {
        return std::max({scr, s16(win + win_offset), s16(gen + win_offset)});
    };
    const auto combined_scissor_value_br = [](s16 scr, s16 win, s16 gen, s16 win_offset) {
        return std::min({scr, s16(win + win_offset), s16(gen + win_offset)});
    };
    const bool enable_offset = !regs.window_scissor.window_offset_disable;

    AmdGpu::Scissor scsr{};
    scsr.top_left_x = combined_scissor_value_tl(
        regs.screen_scissor.top_left_x, s16(regs.window_scissor.top_left_x),
        s16(regs.generic_scissor.top_left_x),
        enable_offset ? regs.window_offset.window_x_offset : 0);
    scsr.top_left_y = combined_scissor_value_tl(
        regs.screen_scissor.top_left_y, s16(regs.window_scissor.top_left_y),
        s16(regs.generic_scissor.top_left_y),
        enable_offset ? regs.window_offset.window_y_offset : 0);
    scsr.bottom_right_x = combined_scissor_value_br(
        regs.screen_scissor.bottom_right_x, regs.window_scissor.bottom_right_x,
        regs.generic_scissor.bottom_right_x,
        enable_offset ? regs.window_offset.window_x_offset : 0);
    scsr.bottom_right_y = combined_scissor_value_br(
        regs.screen_scissor.bottom_right_y, regs.window_scissor.bottom_right_y,
        regs.generic_scissor.bottom_right_y,
        enable_offset ? regs.window_offset.window_y_offset : 0);

    boost::container::static_vector<vk::Viewport, AmdGpu::NUM_VIEWPORTS> viewports;
    boost::container::static_vector<vk::Rect2D, AmdGpu::NUM_VIEWPORTS> scissors;

    if (regs.polygon_control.enable_window_offset &&
        (regs.window_offset.window_x_offset != 0 || regs.window_offset.window_y_offset != 0)) {
        LOG_ERROR(Render_Vulkan,
                  "PA_SU_SC_MODE_CNTL.VTX_WINDOW_OFFSET_ENABLE support is not yet implemented.");
    }

    const auto& vp_ctl = regs.viewport_control;
    u32 viewport_count = 1;
    for (u32 i = 0; i < AmdGpu::NUM_VIEWPORTS; i++) {
        if (regs.viewports[i].xscale != 0.f) {
            viewport_count = i + 1;
        }
    }
    for (u32 i = 0; i < viewport_count; i++) {
        const auto& vp = regs.viewports[i];
        const auto& vp_d = regs.viewport_depths[i];
        if (vp.xscale == 0) {
            // ViewportIndex refers to the register slot, not to a compacted list of active
            // viewports. Keep disabled slots so subsequent eyes retain their indices.
            viewports.push_back({.width = 1.f, .height = 1.f, .maxDepth = 1.f});
            scissors.push_back({.offset = {0, 0}, .extent = {0, 0}});
            continue;
        }

        const auto zoffset = vp_ctl.zoffset_enable ? vp.zoffset : 0.f;
        const auto zscale = vp_ctl.zscale_enable ? vp.zscale : 1.f;

        vk::Viewport viewport{};

        // https://gitlab.freedesktop.org/mesa/mesa/-/blob/209a0ed/src/amd/vulkan/radv_pipeline_graphics.c#L688-689
        // https://gitlab.freedesktop.org/mesa/mesa/-/blob/209a0ed/src/amd/vulkan/radv_cmd_buffer.c#L3103-3109
        // When the clip space is ranged [-1...1], the zoffset is centered.
        // By reversing the above viewport calculations, we get the following:
        if (regs.clipper_control.clip_space == AmdGpu::ClipSpace::MinusWToW) {
            viewport.minDepth = zoffset - zscale;
            viewport.maxDepth = zoffset + zscale;
        } else {
            viewport.minDepth = zoffset;
            viewport.maxDepth = zoffset + zscale;
        }

        if (!instance.IsDepthRangeUnrestrictedSupported()) {
            // Unrestricted depth range not supported by device. Restrict to valid range.
            viewport.minDepth = std::max(viewport.minDepth, 0.f);
            viewport.maxDepth = std::min(viewport.maxDepth, 1.f);
        }

        if (regs.IsClipDisabled()) {
            // In case if clipping is disabled we patch the shader to convert vertex position
            // from screen space coordinates to NDC by defining a render space as full hardware
            // window range [0..16383, 0..16383] and setting the viewport to its size.
            viewport.x = 0.f;
            viewport.y = 0.f;
            viewport.width = float(std::min<u32>(instance.GetMaxViewportWidth(), 16_KB));
            viewport.height = float(std::min<u32>(instance.GetMaxViewportHeight(), 16_KB));
        } else {
            const auto xoffset = vp_ctl.xoffset_enable ? vp.xoffset : 0.f;
            const auto xscale = vp_ctl.xscale_enable ? vp.xscale : 1.f;
            const auto yoffset = vp_ctl.yoffset_enable ? vp.yoffset : 0.f;
            const auto yscale = vp_ctl.yscale_enable ? vp.yscale : 1.f;

            viewport.x = xoffset - xscale;
            viewport.y = yoffset - yscale;
            viewport.width = xscale * 2.0f;
            viewport.height = yscale * 2.0f;
        }

        viewports.push_back(viewport);

        auto vp_scsr = scsr;
        if (regs.mode_control.vport_scissor_enable) {
            vp_scsr.top_left_x =
                std::max(vp_scsr.top_left_x, s16(regs.viewport_scissors[i].top_left_x));
            vp_scsr.top_left_y =
                std::max(vp_scsr.top_left_y, s16(regs.viewport_scissors[i].top_left_y));
            vp_scsr.bottom_right_x = std::min(AmdGpu::Scissor::Clamp(vp_scsr.bottom_right_x),
                                              regs.viewport_scissors[i].bottom_right_x);
            vp_scsr.bottom_right_y = std::min(AmdGpu::Scissor::Clamp(vp_scsr.bottom_right_y),
                                              regs.viewport_scissors[i].bottom_right_y);
        }
        scissors.push_back({
            .offset = {AmdGpu::Scissor::Clamp(vp_scsr.top_left_x),
                       AmdGpu::Scissor::Clamp(vp_scsr.top_left_y)},
            .extent = {vp_scsr.GetWidth(), vp_scsr.GetHeight()},
        });
    }

    // A measuring aid, not a setting: SHADPS4_DBG_VIEWPORT_SCALE=<0..1> shrinks everything that is
    // drawn towards the corner of its target. The picture is wrong, but how much faster the GPU
    // gets tells how much of its time goes into filling pixels rather than into the draws
    // themselves.
    static const float debug_scale = [] {
        const char* value = std::getenv("SHADPS4_DBG_VIEWPORT_SCALE");
        const float scale = value != nullptr ? static_cast<float>(std::atof(value)) : 1.0f;
        return scale > 0.0f && scale < 1.0f ? scale : 1.0f;
    }();
    if (debug_scale != 1.0f) {
        for (auto& viewport : viewports) {
            viewport.x *= debug_scale;
            viewport.y *= debug_scale;
            viewport.width *= debug_scale;
            viewport.height *= debug_scale;
        }
        for (auto& scissor : scissors) {
            scissor.offset.x = static_cast<s32>(scissor.offset.x * debug_scale);
            scissor.offset.y = static_cast<s32>(scissor.offset.y * debug_scale);
            if (scissor.extent.width != 0) {
                scissor.extent.width =
                    std::max(static_cast<u32>(scissor.extent.width * debug_scale), 1u);
            }
            if (scissor.extent.height != 0) {
                scissor.extent.height =
                    std::max(static_cast<u32>(scissor.extent.height * debug_scale), 1u);
            }
        }
    }

    auto& dynamic_state = scheduler.GetDynamicState();
    dynamic_state.SetViewports(viewports);
    dynamic_state.SetScissors(scissors);
}

void Rasterizer::UpdateDepthStencilState() const {
    const auto& regs = liverpool->regs;
    auto& dynamic_state = scheduler.GetDynamicState();

    static const bool dbg_no_depth = std::getenv("SHADPS4_DBG_NO_DEPTH") != nullptr;
    static const bool dbg_no_stencil = std::getenv("SHADPS4_DBG_NO_STENCIL") != nullptr;
    const auto depth_test_enabled =
        regs.depth_control.depth_enable && regs.depth_buffer.DepthValid() && !dbg_no_depth;
    dynamic_state.SetDepthTestEnabled(depth_test_enabled);
    if (depth_test_enabled) {
        dynamic_state.SetDepthWriteEnabled(regs.depth_control.depth_write_enable &&
                                           !regs.depth_render_control.depth_clear_enable);
        dynamic_state.SetDepthCompareOp(LiverpoolToVK::CompareOp(regs.depth_control.depth_func));
    }

    const auto depth_bounds_test_enabled = regs.depth_control.depth_bounds_enable;
    dynamic_state.SetDepthBoundsTestEnabled(depth_bounds_test_enabled);
    if (depth_bounds_test_enabled) {
        dynamic_state.SetDepthBounds(regs.depth_bounds_min, regs.depth_bounds_max);
    }

    const auto depth_bias_enabled = regs.polygon_control.NeedsBias();
    dynamic_state.SetDepthBiasEnabled(depth_bias_enabled);
    if (depth_bias_enabled) {
        const bool front = regs.polygon_control.enable_polygon_offset_front;
        dynamic_state.SetDepthBias(
            front ? regs.poly_offset.front_offset : regs.poly_offset.back_offset,
            regs.poly_offset.depth_bias,
            (front ? regs.poly_offset.front_scale : regs.poly_offset.back_scale) / 16.f);
    }

    const auto stencil_test_enabled =
        regs.depth_control.stencil_enable && regs.depth_buffer.StencilValid() && !dbg_no_stencil;
    dynamic_state.SetStencilTestEnabled(stencil_test_enabled);
    if (stencil_test_enabled) {
        if (StatikStencilClearDraw(regs)) {
            const StencilOps clear_ops{
                .fail_op = vk::StencilOp::eReplace,
                .pass_op = vk::StencilOp::eReplace,
                .depth_fail_op = vk::StencilOp::eReplace,
                .compare_op = vk::CompareOp::eAlways,
            };
            dynamic_state.SetStencilOps(clear_ops, clear_ops);
            dynamic_state.SetStencilReferences(regs.stencil_clear, regs.stencil_clear);
            dynamic_state.SetStencilWriteMasks(0xff, 0xff);
            dynamic_state.SetStencilCompareMasks(0xff, 0xff);
            static bool reported = false;
            if (!reported) {
                reported = true;
                LOG_INFO(Render_Vulkan, "Statik stencil clear preserves guest rectangle coverage and the other eye");
            }
            return;
        }
        const StencilOps front_ops{
            .fail_op = LiverpoolToVK::StencilOp(regs.stencil_control.stencil_fail_front),
            .pass_op = LiverpoolToVK::StencilOp(regs.stencil_control.stencil_zpass_front),
            .depth_fail_op = LiverpoolToVK::StencilOp(regs.stencil_control.stencil_zfail_front),
            .compare_op = LiverpoolToVK::CompareOp(regs.depth_control.stencil_ref_func),
        };
        const StencilOps back_ops = regs.depth_control.backface_enable ? StencilOps{
            .fail_op = LiverpoolToVK::StencilOp(regs.stencil_control.stencil_fail_back),
            .pass_op = LiverpoolToVK::StencilOp(regs.stencil_control.stencil_zpass_back),
            .depth_fail_op = LiverpoolToVK::StencilOp(regs.stencil_control.stencil_zfail_back),
            .compare_op = LiverpoolToVK::CompareOp(regs.depth_control.stencil_bf_func),
        } : front_ops;
        dynamic_state.SetStencilOps(front_ops, back_ops);

        const bool stencil_clear = regs.depth_render_control.stencil_clear_enable;
        const auto front = regs.stencil_ref_front;
        const auto back =
            regs.depth_control.backface_enable ? regs.stencil_ref_back : regs.stencil_ref_front;
        dynamic_state.SetStencilReferences(front.stencil_test_val, back.stencil_test_val);
        dynamic_state.SetStencilWriteMasks(!stencil_clear ? front.stencil_write_mask : 0U,
                                           !stencil_clear ? back.stencil_write_mask : 0U);
        dynamic_state.SetStencilCompareMasks(front.stencil_mask, back.stencil_mask);
    }
}

void Rasterizer::UpdatePrimitiveState(const bool is_indexed) const {
    const auto& regs = liverpool->regs;
    auto& dynamic_state = scheduler.GetDynamicState();

    const auto is_list_topology = [](const AmdGpu::PrimitiveType type) {
        const auto topology = LiverpoolToVK::PrimitiveType(type);
        return topology == vk::PrimitiveTopology::ePointList ||
               topology == vk::PrimitiveTopology::eLineList ||
               topology == vk::PrimitiveTopology::eTriangleList ||
               topology == vk::PrimitiveTopology::eLineListWithAdjacency ||
               topology == vk::PrimitiveTopology::eTriangleListWithAdjacency;
    };
    const auto is_patch_list_topology = [](const AmdGpu::PrimitiveType type) {
        // Quad and rect lists are emulated using tessellation.
        return type == AmdGpu::PrimitiveType::PatchPrimitive ||
               type == AmdGpu::PrimitiveType::QuadList || type == AmdGpu::PrimitiveType::RectList;
    };

    const auto prim_restart =
        (regs.enable_primitive_restart & 1) != 0 &&
        (instance.IsListRestartSupported() || !is_list_topology(regs.primitive_type)) &&
        (instance.IsPatchListRestartSupported() || !is_patch_list_topology(regs.primitive_type));
    ASSERT_MSG(!is_indexed || !prim_restart || regs.primitive_restart_index == 0xFFFF ||
                   regs.primitive_restart_index == 0xFFFFFFFF,
               "Primitive restart index other than -1 is not supported yet");

    const auto cull_mode = LiverpoolToVK::IsPrimitiveCulled(regs.primitive_type)
                               ? LiverpoolToVK::CullMode(regs.polygon_control.CullingMode())
                               : vk::CullModeFlagBits::eNone;
    const auto front_face = LiverpoolToVK::FrontFace(regs.polygon_control.front_face);

    dynamic_state.SetPrimitiveRestartEnabled(prim_restart);
    dynamic_state.SetRasterizerDiscardEnabled(regs.clipper_control.dx_rasterization_kill);
    dynamic_state.SetCullMode(cull_mode);
    dynamic_state.SetFrontFace(front_face);
}

void Rasterizer::UpdateRasterizationState() const {
    const auto& regs = liverpool->regs;
    auto& dynamic_state = scheduler.GetDynamicState();
    dynamic_state.SetLineWidth(regs.line_control.Width());
}

void Rasterizer::UpdateColorBlendingState(const GraphicsPipeline* pipeline) const {
    const auto& regs = liverpool->regs;
    auto& dynamic_state = scheduler.GetDynamicState();
    dynamic_state.SetBlendConstants(regs.blend_constants);
    dynamic_state.SetColorWriteMasks(pipeline->GetGraphicsKey().write_masks);
    dynamic_state.SetAttachmentFeedbackLoopEnabled(attachment_feedback_loop);
}

void Rasterizer::ScopeMarkerBegin(const std::string_view& str, bool from_guest) {
    if (from_guest && TraceDraw()) {
        LOG_INFO(Render_Vulkan, "TRACE marker {}", str);
    }
    if ((from_guest && !EmulatorSettings.IsVkGuestMarkersEnabled()) ||
        (!from_guest && !EmulatorSettings.IsVkHostMarkersEnabled())) {
        return;
    }
    const auto cmdbuf = scheduler.CommandBuffer();
    cmdbuf.beginDebugUtilsLabelEXT(vk::DebugUtilsLabelEXT{
        .pLabelName = str.data(),
    });
}

void Rasterizer::ScopeMarkerEnd(bool from_guest) {
    if ((from_guest && !EmulatorSettings.IsVkGuestMarkersEnabled()) ||
        (!from_guest && !EmulatorSettings.IsVkHostMarkersEnabled())) {
        return;
    }
    const auto cmdbuf = scheduler.CommandBuffer();
    cmdbuf.endDebugUtilsLabelEXT();
}

void Rasterizer::ScopedMarkerInsert(const std::string_view& str, bool from_guest) {
    if ((from_guest && !EmulatorSettings.IsVkGuestMarkersEnabled()) ||
        (!from_guest && !EmulatorSettings.IsVkHostMarkersEnabled())) {
        return;
    }
    const auto cmdbuf = scheduler.CommandBuffer();
    cmdbuf.insertDebugUtilsLabelEXT(vk::DebugUtilsLabelEXT{
        .pLabelName = str.data(),
    });
}

void Rasterizer::ScopedMarkerInsertColor(const std::string_view& str, const u32 color,
                                         bool from_guest) {
    if ((from_guest && !EmulatorSettings.IsVkGuestMarkersEnabled()) ||
        (!from_guest && !EmulatorSettings.IsVkHostMarkersEnabled())) {
        return;
    }
    const auto cmdbuf = scheduler.CommandBuffer();
    cmdbuf.insertDebugUtilsLabelEXT(vk::DebugUtilsLabelEXT{
        .pLabelName = str.data(),
        .color = std::array<f32, 4>(
            {(f32)((color >> 16) & 0xff) / 255.0f, (f32)((color >> 8) & 0xff) / 255.0f,
             (f32)(color & 0xff) / 255.0f, (f32)((color >> 24) & 0xff) / 255.0f})});
}

} // namespace Vulkan
