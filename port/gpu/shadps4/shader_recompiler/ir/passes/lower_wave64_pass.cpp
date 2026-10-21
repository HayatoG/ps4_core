// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <optional>
#include <string>
#include "common/backports.h"
#include "common/logging/classes.h"
#include "shader_recompiler/info.h"
#include "shader_recompiler/ir/basic_block.h"
#include "shader_recompiler/ir/breadth_first_search.h"
#include "shader_recompiler/ir/ir_emitter.h"
#include "shader_recompiler/ir/program.h"
#include "shader_recompiler/profile.h"
#include "shader_recompiler/recompiler_toggles.h"

namespace Shader::Optimization {

static bool IsAtomicWithResult(IR::Opcode op) {
    return (op >= IR::Opcode::SharedAtomicIAdd32 && op <= IR::Opcode::SharedAtomicCmpSwap64) ||
           (op >= IR::Opcode::BufferAtomicIAdd32 && op <= IR::Opcode::BufferAtomicFCmpSwap32) ||
           (op >= IR::Opcode::ImageAtomicIAdd32 && op <= IR::Opcode::ImageAtomicCmpSwap32);
}

bool IsLaneVaryingSource(const IR::Inst& inst) {
    const IR::Opcode op = inst.GetOpcode();
    if (op == IR::Opcode::GetAttributeU32 &&
        inst.Arg(0).Attribute() == IR::Attribute::LocalInvocationId) {
        return true;
    }
    if (!Toggles::LaneDivergence()) {
        return false;
    }
    if (op == IR::Opcode::GetAttributeU32) {
        const IR::Attribute attr = inst.Arg(0).Attribute();
        return attr == IR::Attribute::LocalInvocationIndex ||
               attr == IR::Attribute::SubgroupLtMask;
    }
    // Every lane of an atomic gets its own old value even at one address (the append counter).
    return op == IR::Opcode::LaneId || IsAtomicWithResult(op);
}

static bool IsDivergentCondition(const IR::U1& condition) {
    if (condition.IsImmediate()) {
        return false;
    }
    const IR::Inst* const condition_inst = condition.Inst();
    return IR::BreadthFirstSearch(condition_inst,
                                  [](const IR::Inst* inst) -> std::optional<bool> {
                                      if (inst->GetOpcode() == IR::Opcode::LaneId ||
                                          IsLaneVaryingSource(*inst)) {
                                          return true;
                                      }
                                      return std::nullopt;
                                  })
        .value_or(false);
}

// tlg: true when a value is the same in every lane of the wave for sure: its inputs reach no
// lane-varying source, no phi (its incoming edge can depend on the lane) and no LDS or memory
// load (another lane may have written it). Such a ReadFirstLane needs no wave64 exchange.
static bool IsProvablyWaveUniform(const IR::Value& value) {
    if (value.IsImmediate()) {
        return true;
    }
    return !IR::BreadthFirstSearch(static_cast<const IR::Inst*>(value.Inst()),
                                   [](const IR::Inst* inst) -> std::optional<bool> {
                                       switch (inst->GetOpcode()) {
                                       case IR::Opcode::Phi:
                                       case IR::Opcode::LaneId:
                                       case IR::Opcode::LoadSharedU16:
                                       case IR::Opcode::LoadSharedU32:
                                       case IR::Opcode::LoadSharedU64:
                                       case IR::Opcode::ImageRead:
                                       case IR::Opcode::ReadLane:
                                       case IR::Opcode::Shuffle:
                                       case IR::Opcode::ShuffleXor:
                                       case IR::Opcode::QuadBroadcast:
                                           return true;
                                       case IR::Opcode::GetAttributeU32:
                                           // Workgroup id and the like are uniform; lane ids not.
                                           return inst->Arg(0).Attribute() ==
                                                      IR::Attribute::LocalInvocationId ||
                                                  inst->Arg(0).Attribute() ==
                                                      IR::Attribute::LocalInvocationIndex ||
                                                  inst->Arg(0).Attribute() ==
                                                      IR::Attribute::SubgroupLtMask;
                                       default:
                                           if (IsAtomicWithResult(inst->GetOpcode()) ||
                                               (inst->GetOpcode() >= IR::Opcode::LoadBufferU8 &&
                                                inst->GetOpcode() <=
                                                    IR::Opcode::LoadBufferFormatF32)) {
                                               return true;
                                           }
                                           return std::nullopt;
                                       }
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

// tlg: also used by SharedMemoryBarrierPass (a constant-lane ReadLane in these blocks is uniform).
std::vector<IR::Block*> FindWave64UniformBlocks(const IR::Program& program) {
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

// tlg: BP06 (SotC 8d84cf7). Lane identity of a reduction step, when value = op(x, ShuffleXor(x, k)).
// Based on the wave64_missing_lane_identity workaround of Pink-shadPS4 (luizgustavs).
static const IR::Inst* SkipBitCasts(const IR::Value& value) {
    if (value.IsImmediate()) {
        return nullptr;
    }
    const IR::Inst* inst = value.Inst();
    while (inst->GetOpcode() == IR::Opcode::BitCastU32F32 ||
           inst->GetOpcode() == IR::Opcode::BitCastF32U32) {
        if (inst->Arg(0).IsImmediate()) {
            return nullptr;
        }
        inst = inst->Arg(0).Inst();
    }
    return inst;
}

static std::optional<u32> ReductionIdentity(const IR::Value& value) {
    const IR::Inst* op = SkipBitCasts(value);
    if (!op || op->NumArgs() != 2) {
        return std::nullopt;
    }
    bool is_step = false;
    for (u32 arg = 0; arg < 2; ++arg) {
        const IR::Inst* shuffle = SkipBitCasts(op->Arg(arg));
        const IR::Inst* other = SkipBitCasts(op->Arg(1 - arg));
        is_step |= shuffle && other && shuffle->GetOpcode() == IR::Opcode::ShuffleXor &&
                   SkipBitCasts(shuffle->Arg(0)) == other;
    }
    if (!is_step) {
        return std::nullopt;
    }
    switch (op->GetOpcode()) {
    case IR::Opcode::FPMin32:
        return 0x7f800000u; // +inf
    case IR::Opcode::FPMax32:
        return 0xff800000u; // -inf
    case IR::Opcode::FPMul32:
        return 0x3f800000u; // 1.0
    case IR::Opcode::UMin32:
    case IR::Opcode::BitwiseAnd32:
        return 0xffffffffu;
    case IR::Opcode::SMin32:
        return 0x7fffffffu;
    case IR::Opcode::SMax32:
        return 0x80000000u;
    case IR::Opcode::IMul32:
        return 1u;
    case IR::Opcode::FPAdd32:
    case IR::Opcode::IAdd32:
    case IR::Opcode::UMax32:
    case IR::Opcode::BitwiseOr32:
    case IR::Opcode::BitwiseXor32:
        return 0u;
    default:
        return std::nullopt;
    }
}

/// tlg: TLG_VERBOSE_LOG=1/2 (the launcher's detailed log), read once. The graphics lane-op census
/// below walks the whole program, so it is skipped entirely unless the detailed log is on.
static bool VerboseLog() {
    static const bool on = [] {
        const char* v = std::getenv("TLG_VERBOSE_LOG");
        return v && std::atoi(v) > 0;
    }();
    return on;
}

/// tlg: lane operations of one program, counted per opcode. The compute path below uses it to name
/// the wave64 candidates on 32-wide hardware; the graphics census prints what a VS/PS gives up when
/// its lane operations run on a 32-lane wave (they are never lowered onto 64 lanes).
struct LaneOpCensus {
    static constexpr size_t NumOps = 12;
    u32 counts[NumOps]{};
    bool lane_math = false; ///< the shader numbers lanes (ballots, prefix masks)

    std::string Describe() const {
        static constexpr const char* names[NumOps]{"Ballot", "GetExec", "SetExec", "GroupAny",
                                                   "ReadFirstLane", "ReadLaneImm", "ReadLaneVar",
                                                   "WriteLane", "Shuffle", "ShuffleXor", "LaneId",
                                                   "LtMask"};
        std::string used;
        for (size_t i = 0; i < NumOps; ++i) {
            if (counts[i]) {
                used += fmt::format("{}{} {}", used.empty() ? "" : ", ", names[i], counts[i]);
            }
        }
        return used;
    }
};

static LaneOpCensus CountLaneOps(IR::Program& program) {
    LaneOpCensus census{};
    for (IR::Block* block : program.blocks) {
        for (IR::Inst& inst : block->Instructions()) {
            switch (inst.GetOpcode()) {
            case IR::Opcode::Ballot: ++census.counts[0]; break;
            case IR::Opcode::GetExec: ++census.counts[1]; break;
            case IR::Opcode::SetExec: ++census.counts[2]; break;
            case IR::Opcode::GroupAny: ++census.counts[3]; break;
            case IR::Opcode::ReadFirstLane: ++census.counts[4]; break;
            case IR::Opcode::ReadLane: ++census.counts[inst.Arg(1).IsImmediate() ? 5 : 6]; break;
            case IR::Opcode::WriteLane: ++census.counts[7]; break;
            case IR::Opcode::Shuffle: ++census.counts[8]; break;
            case IR::Opcode::ShuffleXor: ++census.counts[9]; break;
            case IR::Opcode::LaneId: ++census.counts[10]; break;
            case IR::Opcode::GetAttributeU32:
                if (inst.Arg(0).Attribute() == IR::Attribute::SubgroupLtMask) ++census.counts[11];
                break;
            default: break;
            }
        }
    }
    census.lane_math = census.counts[0] || census.counts[11];
    return census;
}

void LowerWave64BallotPass(IR::Program& program, const RuntimeInfo& runtime_info,
                           const Profile& profile) {
    if (program.info.hw_stage != HwStage::Compute) {
        // tlg: census of the graphics stages, which never get the wave64 lowering below (it is
        // compute-only): their lane operations keep running on 32 lanes, so on a 64-lane host the
        // other half of the wave is lost. TLG_VERBOSE_LOG only, no change to the graphics IR.
        if (VerboseLog()) {
            const std::string used = CountLaneOps(program).Describe();
            if (!used.empty()) {
                LOG_WARNING(Render_Recompiler,
                            "tlg: {} shader {:#x} lane ops (not lowered for wave64): {}",
                            program.info.hw_stage, program.info.pgm_hash, used);
            }
        }
        return;
    }
    const bool bp06 = Common::Backports::On(Common::Backports::Id::MaximalReconvergence);
    if (profile.subgroup_size == 64 && !bp06) {
        return;
    }

    const auto [size_x, size_y, size_z] = runtime_info.hw.cs.workgroup_size;
    const u32 num_threads = size_x * size_y * size_z;
    if (num_threads <= 32) {
        if (!bp06) {
            return;
        }
        // tlg: BP06 (SotC 8d84cf7). The whole workgroup fits in one host subgroup, but the guest
        // code was written for a wave64 whose upper half is inactive. A read of a fixed lane in
        // that upper half (the "readlane 32" that merges the two halves of a cross-lane
        // reduction) indexes past the populated lanes, which is undefined for the broadcast -
        // on 64-lane hosts too (lanes 32-63 have no invocation). When the value read is a
        // reduction step the missing half is the reduction identity (folding it onto the lower
        // half would count the lower lanes twice); other reads fold into the lower half.
        const u32 host_lanes = std::min<u32>(profile.subgroup_size, 32);
        if (host_lanes == 0) {
            return;
        }
        for (IR::Block* block : program.blocks) {
            for (IR::Inst& inst : block->Instructions()) {
                if (inst.GetOpcode() != IR::Opcode::ReadLane || !inst.Arg(1).IsImmediate()) {
                    continue;
                }
                const u32 lane = inst.Arg(1).U32();
                if (lane < host_lanes) {
                    continue;
                }
                Common::Backports::Hit(Common::Backports::Id::MaximalReconvergence);
                if (const auto identity = ReductionIdentity(inst.Arg(0))) {
                    inst.ReplaceUsesWithAndRemove(IR::Value{*identity});
                } else {
                    inst.SetArg(1, IR::Value{lane % host_lanes});
                }
            }
        }
        return;
    }
    if (profile.subgroup_size == 64) {
        return;
    }

    // tlg: one line per compute shader wider than a 32-lane wave that uses lane operations:
    // the candidates when wave64 code misbehaves on 32-wide hardware (flickering lights).
    const auto census = CountLaneOps(program);
    const bool lane_math = census.lane_math;
    const std::string lane_ops = census.Describe();
    if (!lane_ops.empty()) {
        LOG_WARNING(Render_Recompiler, "tlg: wave64 compute shader {:#x} ({} threads, {}x{}x{}): {}",
                    program.info.pgm_hash, num_threads, size_x, size_y, size_z, lane_ops);
    }

    std::vector<IR::Inst*> worklist;
    std::vector<IR::Inst*> divergent_ballots;
    const auto uniform_blocks = FindWave64UniformBlocks(program);
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
            } else if (inst.GetOpcode() == IR::Opcode::ReadFirstLane && is_uniform &&
                       (lane_math ||
                        (Toggles::ReadFirstLaneAll() && !IsProvablyWaveUniform(inst.Arg(0))))) {
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
            // tlg: both 32-lane halves read their wave's pair of slots (bits 0..31 from the even
            // subgroup, 32..63 from the odd one); the odd half used to read its own slot and the
            // next wave's, so its lanes counted against the wrong mask (and the last subgroup
            // read past the scratch).
            const IR::U64 mask =
                IR::U64{ir.LoadShared(64, false, ir.BitwiseAnd(offset, ir.Imm32(~7u)))};
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
