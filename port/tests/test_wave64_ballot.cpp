// SPDX-License-Identifier: GPL-2.0-or-later
// wave64 ballot lowering on a 32-wide host (from infamous_pc e51a229). No window, no game.
//   ninja -C out/gpu wave64-ballot-test && out/gpu/wave64-ballot-test
#include <cstdio>
#include "shader_recompiler/ir/ir_emitter.h"
#include "shader_recompiler/ir/passes/ir_passes.h"
#include "shader_recompiler/profile.h"
#include "shader_recompiler/runtime_info.h"

using namespace Shader;

// libtlggpu.so calls into the runtime; test_gpu_runtime_stubs.cpp has the rest (weak: some ports stub it there).
extern "C" __attribute__((weak)) void runtime_report(void) {}

// A 64-thread compute block with one ballot in uniform flow, on a 32-wide host: both 32-lane
// halves must read their wave's pair of LDS slots, so the 64-bit load's address is the slot
// address with bit 2 cleared (the odd half used to read its own slot and the next wave's).
static bool BallotReadsSlotPair() {
    Info info{};
    info.hw_stage = HwStage::Compute;
    info.sw_stage = SwStage::Compute;
    RuntimeInfo runtime{};
    runtime.Initialize(HwStage::Compute, SwStage::Compute);
    runtime.hw.cs.shared_memory_size = 0;
    runtime.hw.cs.workgroup_size = {64, 1, 1};
    Common::ObjectPool<IR::Inst> pool;
    IR::Block block(pool);
    IR::IREmitter ir(block);
    const IR::U32 x = ir.GetAttributeU32(IR::Attribute::LocalInvocationId, 0);
    const IR::U64 mask = ir.Ballot(ir.INotEqual(ir.BitwiseAnd(x, ir.Imm32(1u)), ir.Imm32(0u)));
    const IR::U32 low{ir.CompositeExtract(ir.UnpackUint2x32(mask), 0)};
    ir.WriteShared(32, low, ir.Imm32(0u));
    ir.Epilogue();
    IR::Program program(info);
    program.blocks.push_back(&block);
    program.post_order_blocks.push_back(&block);
    program.syntax_list.push_back({.data = {.block = &block},
                                   .type = IR::AbstractSyntaxNode::Type::Block});
    program.syntax_list.push_back({.type = IR::AbstractSyntaxNode::Type::Return});
    Profile profile{};
    profile.subgroup_size = 32;
    Optimization::LowerWave64BallotPass(program, runtime, profile);
    u32 loads = 0;
    bool paired = true;
    for (const IR::Inst& inst : block.Instructions()) {
        if (inst.GetOpcode() != IR::Opcode::LoadSharedU64) {
            continue;
        }
        ++loads;
        const IR::Value address = inst.Arg(0);
        const IR::Inst* op = address.IsImmediate() ? nullptr : address.Inst();
        paired = paired && op && op->GetOpcode() == IR::Opcode::BitwiseAnd32 &&
                 op->Arg(1).IsImmediate() && op->Arg(1).U32() == ~7u;
    }
    return loads == 1 && paired;
}

int main() {
    const bool ok = BallotReadsSlotPair();
    std::printf("%s: wave64 ballot on a 32-wide host: both halves read the slot pair\n",
                ok ? "ok" : "FAIL");
    return ok ? 0 : 1;
}
