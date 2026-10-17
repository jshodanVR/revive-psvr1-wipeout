// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <gtest/gtest.h>
#include <spirv/unified1/spirv.hpp11>

#include "shader_recompiler/backend/spirv/emit_spirv.h"
#include "shader_recompiler/frontend/translate/translate.h"
#include "shader_recompiler/ir/ir_emitter.h"
#include "shader_recompiler/ir/passes/ir_passes.h"
#include "shader_recompiler/ir/program.h"
#include "shader_recompiler/recompiler.h"
#include "shader_recompiler/runtime_info.h"

namespace {

using namespace Shader;

TEST(GeometryInputs, InvocationIdReachesV7) {
    Info info{};
    info.stage = Stage::Geometry;
    info.l_stage = LogicalStage::Geometry;
    RuntimeInfo runtime{};
    runtime.Initialize(Stage::Geometry);
    runtime.gs_info.num_invocations = 2;
    Profile profile{};
    Pools pools;
    auto* block = pools.block_pool.Create(pools.inst_pool);
    Gcn::Translator translator(info, runtime, profile);
    translator.EmitPrologue(block);

    const auto store = std::ranges::find_if(block->Instructions(), [](const IR::Inst& inst) {
        return inst.GetOpcode() == IR::Opcode::SetVectorRegister &&
               inst.Arg(0).VectorReg() == IR::VectorReg::V7;
    });
    ASSERT_NE(store, block->end());
    const auto* value = store->Arg(1).Inst();
    ASSERT_EQ(value->GetOpcode(), IR::Opcode::GetAttributeU32);
    EXPECT_EQ(value->Arg(0).Attribute(), IR::Attribute::InvocationId);
}

TEST(GeometryInputs, InputVerticesDoNotDependOnOutputTopology) {
    for (auto output :
         {AmdGpu::GsOutputPrimitiveType::PointList, AmdGpu::GsOutputPrimitiveType::LineStrip,
          AmdGpu::GsOutputPrimitiveType::TriangleStrip}) {
        Info info{};
        info.stage = Stage::Geometry;
        info.l_stage = LogicalStage::Geometry;
        RuntimeInfo runtime{};
        runtime.Initialize(Stage::Geometry);
        runtime.gs_info.in_primitive = AmdGpu::PrimitiveType::AdjTriangleList;
        runtime.gs_info.out_primitive[0] = output;
        Profile profile{};
        Pools pools;
        auto* block = pools.block_pool.Create(pools.inst_pool);
        Gcn::Translator translator(info, runtime, profile);
        translator.EmitPrologue(block);
        constexpr std::array offsets = {IR::VectorReg::V0, IR::VectorReg::V1, IR::VectorReg::V3,
                                        IR::VectorReg::V4, IR::VectorReg::V5, IR::VectorReg::V6};
        for (u32 vertex = 0; vertex < offsets.size(); ++vertex) {
            const auto store =
                std::ranges::find_if(block->Instructions(), [&](const IR::Inst& inst) {
                    return inst.GetOpcode() == IR::Opcode::SetVectorRegister &&
                           inst.Arg(0).VectorReg() == offsets[vertex];
                });
            ASSERT_NE(store, block->end());
            ASSERT_TRUE(store->Arg(1).IsImmediate());
            EXPECT_EQ(store->Arg(1).U32(), vertex);
        }
    }
}

TEST(GeometryInputs, SpirvDeclaresAndLoadsInvocationIdForStereo) {
    Info info{};
    info.stage = Stage::Geometry;
    info.l_stage = LogicalStage::Geometry;
    IR::Program program(info);
    Pools pools;
    auto* block = pools.block_pool.Create(pools.inst_pool);
    program.blocks.push_back(block);
    program.post_order_blocks.push_back(block);
    program.syntax_list.emplace_back();
    program.syntax_list.back().type = IR::AbstractSyntaxNode::Type::Block;
    program.syntax_list.back().data.block = block;
    program.syntax_list.emplace_back();
    program.syntax_list.back().type = IR::AbstractSyntaxNode::Type::Return;

    IR::IREmitter ir(*block);
    ir.Prologue();
    // A stereo GS chooses an output viewport from its invocation ID.
    const auto eye = ir.GetAttributeU32(IR::Attribute::InvocationId);
    ir.SetAttribute(IR::Attribute::ViewportIndex, ir.BitCast<IR::F32>(eye));
    for (u32 comp = 0; comp < 4; ++comp) {
        ir.SetAttribute(IR::Attribute::Position0, ir.Imm32(comp == 3 ? 1.f : 0.f), comp);
    }
    ir.EmitVertex();
    Profile profile{};
    profile.supported_spirv = 0x00010600;
    RuntimeInfo runtime{};
    runtime.Initialize(Stage::Geometry);
    runtime.gs_info.num_invocations = 2;
    runtime.gs_info.output_vertices = 1;
    runtime.gs_info.in_vertex_data_size = 4;
    runtime.gs_info.in_primitive = AmdGpu::PrimitiveType::PointList;
    runtime.gs_info.out_primitive[0] = AmdGpu::GsOutputPrimitiveType::PointList;
    Optimization::CollectShaderInfoPass(program, profile);
    Backend::Bindings bindings{};
    const auto spirv = Backend::SPIRV::EmitSPIRV(profile, runtime, program, bindings);

    u32 invocation_variable = 0;
    bool loaded = false;
    bool stereo_invocations = false;
    for (size_t at = 5; at < spirv.size();) {
        const u32 count = spirv[at] >> 16;
        ASSERT_GT(count, 0u);
        ASSERT_LE(at + count, spirv.size());
        const auto opcode = static_cast<spv::Op>(spirv[at] & 0xffff);
        if (opcode == spv::Op::OpDecorate && count == 4 &&
            spirv[at + 2] == u32(spv::Decoration::BuiltIn) &&
            spirv[at + 3] == u32(spv::BuiltIn::InvocationId)) {
            invocation_variable = spirv[at + 1];
        }
        if (opcode == spv::Op::OpExecutionMode && count == 4 &&
            spirv[at + 2] == u32(spv::ExecutionMode::Invocations)) {
            stereo_invocations = spirv[at + 3] == 2;
        }
        if (opcode == spv::Op::OpLoad && count >= 4 && invocation_variable != 0 &&
            spirv[at + 3] == invocation_variable) {
            loaded = true;
        }
        at += count;
    }
    EXPECT_NE(invocation_variable, 0u);
    EXPECT_TRUE(loaded);
    EXPECT_TRUE(stereo_invocations);
}

TEST(GeometrySpecialization, EquivalentLayoutsMatchIncludingZeroInvocations) {
    GeometryRuntimeInfo info{};
    EXPECT_EQ(info, info);
    info.num_invocations = 2;
    EXPECT_EQ(info, info);
}

TEST(GeometrySpecialization, DifferentInvocationsAndRingLayoutsDoNotMatch) {
    GeometryRuntimeInfo base{};
    base.num_invocations = 1;
    base.in_vertex_data_size = 4;
    base.out_vertex_data_size = 8;
    auto changed = base;
    changed.num_invocations = 2;
    EXPECT_NE(base, changed);
    changed = base;
    changed.in_vertex_data_size = 8;
    EXPECT_NE(base, changed);
    changed = base;
    changed.out_vertex_data_size = 12;
    EXPECT_NE(base, changed);
    changed = base;
    changed.mode = AmdGpu::GsScenario::ScenarioG;
    EXPECT_NE(base, changed);
}

} // namespace
