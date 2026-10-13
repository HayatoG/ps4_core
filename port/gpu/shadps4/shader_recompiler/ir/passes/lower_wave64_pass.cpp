// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <string>
#include "common/logging/classes.h"
#include "shader_recompiler/info.h"
#include "shader_recompiler/ir/basic_block.h"
#include "shader_recompiler/ir/breadth_first_search.h"
#include "shader_recompiler/ir/ir_emitter.h"
#include "shader_recompiler/ir/program.h"
#include "shader_recompiler/profile.h"

namespace Shader::Optimization {

static bool IsDivergentCondition(const IR::U1& condition) {
    if (condition.IsImmediate()) {
        return false;
    }
    const IR::Inst* const condition_inst = condition.Inst();
    return IR::BreadthFirstSearch(condition_inst,
                                  [](const IR::Inst* inst) -> std::optional<bool> {
                                      switch (inst->GetOpcode()) {
                                      case IR::Opcode::LaneId:
                                          return true;
                                      case IR::Opcode::GetAttributeU32:
                                          if (inst->Arg(0).Attribute() ==
                                              IR::Attribute::LocalInvocationId) {
                                              return true;
                                          }
                                          break;
                                      default:
                                          break;
                                      }
                                      return std::nullopt;
                                  })
        .value_or(false);
}

// tlg: loops whose exit depends on the lane are divergent; the others run the same iterations
// in every lane (e.g. over a light list), so ballots inside can be lowered with barriers, the
// same reasoning SharedMemoryBarrierPass applies. Before, every ballot inside any loop stayed a
// 32-lane ballot on 32-wide hardware: half the wave64 lanes were lost (inFAMOUS particle and
// light passes, seen as flickering smoke and fire).
static std::vector<const IR::Block*> FindDivergentLoops(const IR::Program& program) {
    std::vector<const IR::Block*> divergent;
    for (const IR::AbstractSyntaxNode& node : program.syntax_list) {
        if (node.type == IR::AbstractSyntaxNode::Type::Break &&
            IsDivergentCondition(node.data.break_node.cond)) {
            divergent.push_back(node.data.break_node.merge);
        }
        if (node.type == IR::AbstractSyntaxNode::Type::Repeat &&
            IsDivergentCondition(node.data.repeat.cond)) {
            divergent.push_back(node.data.repeat.merge);
        }
    }
    return divergent;
}

static std::vector<IR::Block*> FindUniformBlocks(const IR::Program& program) {
    using Type = IR::AbstractSyntaxNode::Type;
    const auto divergent_loops = FindDivergentLoops(program);

    struct ConditionalScope {
        const IR::Block* merge;
        bool divergent;
    };

    std::vector<IR::Block*> blocks;
    std::vector<ConditionalScope> conditionals;
    std::vector<const IR::Block*> loops;
    u32 divergence_depth{};
    for (const IR::AbstractSyntaxNode& node : program.syntax_list) {
        switch (node.type) {
        case Type::If: {
            const bool divergent = IsDivergentCondition(node.data.if_node.cond);
            conditionals.push_back({node.data.if_node.merge, divergent});
            divergence_depth += static_cast<u32>(divergent);
            break;
        }
        case Type::EndIf:
            ASSERT(!conditionals.empty() && conditionals.back().merge == node.data.end_if.merge);
            divergence_depth -= static_cast<u32>(conditionals.back().divergent);
            conditionals.pop_back();
            break;
        case Type::Loop:
            loops.push_back(node.data.loop.merge);
            divergence_depth += static_cast<u32>(std::ranges::contains(divergent_loops, node.data.loop.merge));
            break;
        case Type::Repeat:
            if (loops.empty() || loops.back() != node.data.repeat.merge) {
                return {};
            }
            divergence_depth -= static_cast<u32>(std::ranges::contains(divergent_loops, loops.back()));
            loops.pop_back();
            break;
        case Type::Block:
            if (divergence_depth == 0) {
                blocks.push_back(node.data.block);
            }
            break;
        default:
            break;
        }
    }
    if (!conditionals.empty() || !loops.empty()) {
        return {};
    }
    return blocks;
}

void LowerWave64BallotPass(IR::Program& program, const RuntimeInfo& runtime_info,
                           const Profile& profile) {
    if (program.info.hw_stage != HwStage::Compute || profile.subgroup_size == 64) {
        return;
    }

    const auto [size_x, size_y, size_z] = runtime_info.hw.cs.workgroup_size;
    const u32 num_threads = size_x * size_y * size_z;
    if (num_threads <= 32) {
        return;
    }

    // tlg: one line per compute shader wider than a 32-lane wave that uses lane operations:
    // the candidates when wave64 code misbehaves on 32-wide hardware (flickering lights).
    bool lane_math = false; // the shader numbers lanes (ballots, prefix masks)
    {
        u32 counts[12]{};
        static constexpr const char* names[12]{"Ballot", "GetExec", "SetExec", "GroupAny",
                                               "ReadFirstLane", "ReadLaneImm", "ReadLaneVar",
                                               "WriteLane", "Shuffle", "ShuffleXor", "LaneId",
                                               "LtMask"};
        for (IR::Block* block : program.blocks) {
            for (IR::Inst& inst : block->Instructions()) {
                switch (inst.GetOpcode()) {
                case IR::Opcode::Ballot: ++counts[0]; break;
                case IR::Opcode::GetExec: ++counts[1]; break;
                case IR::Opcode::SetExec: ++counts[2]; break;
                case IR::Opcode::GroupAny: ++counts[3]; break;
                case IR::Opcode::ReadFirstLane: ++counts[4]; break;
                case IR::Opcode::ReadLane: ++counts[inst.Arg(1).IsImmediate() ? 5 : 6]; break;
                case IR::Opcode::WriteLane: ++counts[7]; break;
                case IR::Opcode::Shuffle: ++counts[8]; break;
                case IR::Opcode::ShuffleXor: ++counts[9]; break;
                case IR::Opcode::LaneId: ++counts[10]; break;
                case IR::Opcode::GetAttributeU32:
                    if (inst.Arg(0).Attribute() == IR::Attribute::SubgroupLtMask) ++counts[11];
                    break;
                default: break;
                }
            }
        }
        lane_math = counts[0] || counts[11];
        std::string used;
        for (int i = 0; i < 12; ++i) {
            if (counts[i]) used += fmt::format("{}{} {}", used.empty() ? "" : ", ", names[i], counts[i]);
        }
        if (!used.empty()) {
            LOG_WARNING(Render_Recompiler, "tlg: wave64 compute shader {:#x} ({} threads, {}x{}x{}): {}",
                        program.info.pgm_hash, num_threads, size_x, size_y, size_z, used);
        }
    }

    std::vector<IR::Inst*> worklist;
    std::vector<IR::Inst*> divergent_ballots;
    const auto uniform_blocks = FindUniformBlocks(program);
    for (IR::Block* block : program.blocks) {
        const bool is_uniform = std::ranges::contains(uniform_blocks, block);
        const auto push_worklist = [&](IR::Inst& inst) {
            if (is_uniform) {
                worklist.push_back(&inst);
            } else {
                if (inst.GetOpcode() == IR::Opcode::Ballot) {
                    divergent_ballots.push_back(&inst);
                } else {
                    LOG_WARNING(Render_Recompiler,
                                "{} instruction in non uniform control flow (shader {:#x})",
                                inst.GetOpcode(), program.info.pgm_hash);
                }
            }
        };
        for (IR::Inst& inst : block->Instructions()) {
            if (inst.GetOpcode() == IR::Opcode::ReadLane && inst.Arg(1).IsImmediate()) {
                push_worklist(inst);
            } else if (inst.GetOpcode() == IR::Opcode::ReadFirstLane && lane_math && is_uniform) {
                // tlg: append pattern: the wave counts its items (ballot / prefix mask), one
                // lane reserves space with an atomic and ReadFirstLane broadcasts the base.
                // On 32-wide hardware the second half read its own first lane, which never
                // reserved anything (inFAMOUS: flickering lights and steam).
                worklist.push_back(&inst);
            } else if (inst.GetOpcode() == IR::Opcode::Ballot) {
                const auto is_unpack = [](const IR::Use& use) {
                    return use.user->GetOpcode() == IR::Opcode::UnpackUint2x32;
                };
                if (std::ranges::any_of(inst.Uses(), is_unpack)) {
                    push_worklist(inst);
                }
            } else if (inst.GetOpcode() == IR::Opcode::GetAttributeU32 &&
                       inst.Arg(0).Attribute() == IR::Attribute::SubgroupLtMask) {
                worklist.push_back(&inst);
            }
        }
    }
    // tlg: a ballot in divergent flow (waterfall loops) cannot gather the other 32-lane half,
    // but SubgroupLtMask below is always in wave64 numbering. Put the 32-lane ballot in its
    // own half of the 64-bit mask so both agree: a lane's prefix count (mbcnt) and the
    // half's popcount then index the same slots. Left in bits 0..31, lanes 32..63 computed
    // their slots against the wrong bits (lost and duplicated entries: inFAMOUS particle and
    // light lists flickered).
    for (IR::Inst* inst : divergent_ballots) {
        IR::IREmitter ir{*inst->GetParent(), IR::Block::InstructionList::s_iterator_to(*inst)};
        const IR::U32 invocation_index = ir.GetAttributeU32(IR::Attribute::LocalInvocationIndex);
        const IR::U1 upper_half =
            ir.INotEqual(ir.BitwiseAnd(ir.ShiftRightLogical(invocation_index, ir.Imm32(5)), ir.Imm32(1)),
                         ir.Imm32(0));
        const IR::U32 bits =
            IR::U32{ir.CompositeExtract(ir.UnpackUint2x32(ir.Ballot(IR::U1{inst->Arg(0)})), 0)};
        const IR::U32 low = IR::U32{ir.Select(upper_half, ir.Imm32(0u), bits)};
        const IR::U32 high = IR::U32{ir.Select(upper_half, bits, ir.Imm32(0u))};
        inst->ReplaceUsesWithAndRemove(ir.PackUint2x32(ir.CompositeConstruct(low, high)));
    }
    if (!divergent_ballots.empty()) {
        LOG_INFO(Render_Recompiler, "shader {:#x}: {} divergent ballots kept per 32-lane half",
                 program.info.pgm_hash, divergent_ballots.size());
    }
    if (worklist.empty()) {
        return;
    }

    const u32 scratch_base = Common::AlignUp(runtime_info.hw.cs.shared_memory_size, sizeof(u64));
    const u32 scratch_size =
        (Common::AlignUp(num_threads, 64) / profile.subgroup_size) * sizeof(u32);
    program.info.shared_memory_scratch_size =
        scratch_base + scratch_size - runtime_info.hw.cs.shared_memory_size;

    for (IR::Inst* inst : worklist) {
        LOG_INFO(Render_Recompiler, "Lowering {} instruction for wave64", inst->GetOpcode());
        IR::IREmitter ir{*inst->GetParent(), IR::Block::InstructionList::s_iterator_to(*inst)};
        const IR::U32 invocation_index = ir.GetAttributeU32(IR::Attribute::LocalInvocationIndex);
        const IR::U32 subgroup_id = ir.ShiftRightLogical(invocation_index, ir.Imm32(5));
        if (inst->GetOpcode() == IR::Opcode::Ballot) {
            const IR::U32 mask_low =
                IR::U32{ir.CompositeExtract(ir.UnpackUint2x32(ir.Ballot(IR::U1{inst->Arg(0)})), 0)};
            const IR::U32 offset =
                ir.IAdd(ir.Imm32(scratch_base), ir.ShiftLeftLogical(subgroup_id, ir.Imm32(2u)));
            ir.WriteShared(32, mask_low, offset);
            ir.Barrier();
            const IR::U64 mask = IR::U64{ir.LoadShared(64, false, offset)};
            ir.Barrier();
            inst->ReplaceUsesWithAndRemove(mask);
        } else if (inst->GetOpcode() == IR::Opcode::ReadLane) {
            const IR::U32 lane32 = ir.BitwiseAnd(IR::U32{inst->Arg(1)}, ir.Imm32(31));
            const IR::U32 half = ir.ShiftRightLogical(IR::U32{inst->Arg(1)}, ir.Imm32(5u));
            const IR::U32 offset =
                ir.IAdd(ir.Imm32(scratch_base), ir.ShiftLeftLogical(subgroup_id, ir.Imm32(2u)));
            ir.WriteShared(32, ir.ReadLane(IR::U32{inst->Arg(0)}, lane32), offset);
            ir.Barrier();
            const IR::U32 value =
                IR::U32{ir.LoadShared(32, false,
                                      ir.IAdd(ir.BitwiseAnd(offset, ir.Imm32(~7u)),
                                              ir.ShiftLeftLogical(half, ir.Imm32(2u))))};
            ir.Barrier();
            inst->ReplaceUsesWithAndRemove(value);
        } else if (inst->GetOpcode() == IR::Opcode::ReadFirstLane) {
            // In uniform flow every lane is active: the wave64's first lane is the first lane of
            // the even 32-lane half.
            const IR::U32 offset =
                ir.IAdd(ir.Imm32(scratch_base), ir.ShiftLeftLogical(subgroup_id, ir.Imm32(2u)));
            ir.WriteShared(32, ir.ReadFirstLane(IR::U32{inst->Arg(0)}), offset);
            ir.Barrier();
            const IR::U32 value =
                IR::U32{ir.LoadShared(32, false, ir.BitwiseAnd(offset, ir.Imm32(~7u)))};
            ir.Barrier();
            inst->ReplaceUsesWithAndRemove(value);
        } else if (inst->GetOpcode() == IR::Opcode::GetAttributeU32 &&
                   inst->Arg(0).Attribute() == IR::Attribute::SubgroupLtMask) {
            const IR::U32 subgroup_invocation_id = ir.BitwiseAnd(invocation_index, ir.Imm32(63));
            const IR::U64 mask = ir.ISub(
                ir.ShiftLeftLogical(ir.Imm64(u64{1}), subgroup_invocation_id), ir.Imm64(u64{1}));
            inst->ReplaceUsesWithAndRemove(
                ir.CompositeExtract(ir.UnpackUint2x32(mask), inst->Arg(1).U32()));
        }
    }
}

} // namespace Shader::Optimization
