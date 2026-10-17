// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <algorithm>
#include <array>
#include <cstring>
#include <map>
#include <span>
#include <vector>
#include <Zydis/Zydis.h>
#include "common/sha256.h"
#include "common/types.h"

namespace Core::KnownTitle::WipeoutHdStack {

constexpr size_t Entry = 0x73fec0, Length = 0x12ae;
constexpr size_t Cave = 0x966b00, Capacity = 0x1500;

// CUSA05670 01.07 HD/Fury vertex conversion leaf. A handled GPU write fault
// overwrites its SysV red zone on Windows: observed R8 spill changed from
// 0x205667d00 to zero, followed by the friend's exact fault at RVA 0x741128.
// Allocate the whole red zone, preserving every stack operand and code target.
// Copy verified user code; never embed a guest function in the distribution.
inline bool Apply(std::span<u8> image) {
    if (image.size() < Cave + Capacity || image.size() < Entry + Length) {
        return false;
    }
    const auto original = image.subspan(Entry, Length);
    if (sha256::SHA256::Hex(std::string_view{
            reinterpret_cast<const char*>(original.data()), original.size()}) !=
            "d7ae6b509322352ce6cda5dd0083d6af62be8af25f381fb3229d9099e63c0c90" ||
        !std::all_of(image.begin() + Cave, image.begin() + Cave + Capacity,
                     [](u8 b) { return b == 0; })) {
        return false;
    }
    struct Instruction {
        size_t offset{}, relocated{};
        std::vector<u8> bytes;
        u64 target{};
        bool branch{}, rip_relative{};
    };
    ZydisDecoder decoder{};
    if (!ZYAN_SUCCESS(ZydisDecoderInit(&decoder, ZYDIS_MACHINE_MODE_LONG_64,
                                      ZYDIS_STACK_WIDTH_64))) {
        return false;
    }
    std::vector<Instruction> instructions;
    std::map<size_t, size_t> offsets;
    size_t output_size = 0, stack_operands = 0, reserves = 0, releases = 0;
    for (size_t at = 0; at < Length;) {
        ZydisDecodedInstruction decoded{};
        ZydisDecodedOperand operands[ZYDIS_MAX_OPERAND_COUNT]{};
        if (!ZYAN_SUCCESS(ZydisDecoderDecodeFull(&decoder, original.data() + at,
                                               Length - at, &decoded, operands))) {
            return false;
        }
        Instruction node{at, output_size,
                         {original.begin() + at, original.begin() + at + decoded.length}};
        ZydisEncoderRequest request{};
        if (!ZYAN_SUCCESS(ZydisEncoderDecodedInstructionToEncoderRequest(
                &decoded, operands, decoded.operand_count_visible, &request))) {
            return false;
        }
        bool encode = false;
        for (size_t i = 0; i < decoded.operand_count_visible; ++i) {
            const auto& op = operands[i];
            if (op.type == ZYDIS_OPERAND_TYPE_MEMORY && op.mem.base == ZYDIS_REGISTER_RSP) {
                // Only the allocated body uses RSP-relative memory in this verified function.
                if (at < 0x11 || at >= 0x129c || op.mem.index != ZYDIS_REGISTER_NONE) {
                    return false;
                }
                request.operands[i].mem.displacement += 128;
                ++stack_operands;
                encode = true;
            }
            if (op.type == ZYDIS_OPERAND_TYPE_MEMORY && op.mem.base == ZYDIS_REGISTER_RIP) {
                if (!ZYAN_SUCCESS(ZydisCalcAbsoluteAddress(&decoded, &op, Entry + at,
                                                          &node.target))) {
                    return false;
                }
                node.rip_relative = true;
            }
            if (op.type == ZYDIS_OPERAND_TYPE_IMMEDIATE && op.imm.is_relative) {
                if (!ZYAN_SUCCESS(ZydisCalcAbsoluteAddress(&decoded, &op, Entry + at,
                                                          &node.target)) ||
                    node.target < Entry || node.target >= Entry + Length) {
                    return false;
                }
                node.branch = true;
                request.branch_type = ZYDIS_BRANCH_TYPE_NEAR;
                request.branch_width = ZYDIS_BRANCH_WIDTH_32;
                request.operands[i].imm.s = 0;
                encode = true;
            }
        }
        if (decoded.operand_count_visible == 2 &&
            operands[0].type == ZYDIS_OPERAND_TYPE_REGISTER &&
            operands[0].reg.value == ZYDIS_REGISTER_RSP) {
            if (operands[1].type != ZYDIS_OPERAND_TYPE_IMMEDIATE ||
                operands[1].imm.value.u != 0xb8) {
                return false;
            }
            if (at == 0xa && decoded.mnemonic == ZYDIS_MNEMONIC_SUB) {
                ++reserves;
            } else if (at == 0x129c && decoded.mnemonic == ZYDIS_MNEMONIC_ADD) {
                ++releases;
            } else {
                return false;
            }
            request.operands[1].imm.u = 0x138;
            encode = true;
        }
        if (encode) {
            std::array<u8, ZYDIS_MAX_INSTRUCTION_LENGTH> buffer{};
            ZyanUSize size = buffer.size();
            if (!ZYAN_SUCCESS(ZydisEncoderEncodeInstruction(&request, buffer.data(), &size))) {
                return false;
            }
            node.bytes.assign(buffer.begin(), buffer.begin() + size);
        }
        offsets.emplace(at, output_size);
        output_size += node.bytes.size();
        instructions.push_back(std::move(node));
        at += decoded.length;
    }
    if (reserves != 1 || releases != 1 || stack_operands == 0 || output_size > Capacity) {
        return false;
    }
    std::vector<u8> relocated;
    relocated.reserve(output_size);
    for (auto& node : instructions) {
        if (node.branch || node.rip_relative) {
            ZydisDecodedInstruction decoded{};
            ZydisDecodedOperand operands[ZYDIS_MAX_OPERAND_COUNT]{};
            if (!ZYAN_SUCCESS(ZydisDecoderDecodeFull(&decoder, node.bytes.data(),
                                                   node.bytes.size(), &decoded, operands))) {
                return false;
            }
            u64 target = node.target;
            if (node.branch) {
                const auto found = offsets.find(target - Entry);
                if (found == offsets.end()) {
                    return false;
                }
                target = Cave + found->second;
            }
            const size_t field = node.branch ? decoded.raw.imm[0].offset : decoded.raw.disp.offset;
            const auto bits = node.branch ? decoded.raw.imm[0].size : decoded.raw.disp.size;
            const s64 displacement = static_cast<s64>(target) -
                                     static_cast<s64>(Cave + node.relocated + node.bytes.size());
            if (bits != 32 || displacement < INT32_MIN || displacement > INT32_MAX ||
                field + sizeof(s32) > node.bytes.size()) {
                return false;
            }
            const s32 relative = static_cast<s32>(displacement);
            std::memcpy(node.bytes.data() + field, &relative, sizeof(relative));
        }
        relocated.insert(relocated.end(), node.bytes.begin(), node.bytes.end());
    }
    // Commit only after the hash, capacity, decoding and every relocation passed.
    std::memcpy(image.data() + Cave, relocated.data(), relocated.size());
    const s32 jump = static_cast<s32>(Cave - Entry - 5);
    image[Entry] = 0xe9;
    std::memcpy(image.data() + Entry + 1, &jump, sizeof(jump));
    return true;
}

} // namespace Core::KnownTitle::WipeoutHdStack
