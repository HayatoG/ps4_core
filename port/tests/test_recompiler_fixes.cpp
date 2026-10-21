// SPDX-License-Identifier: GPL-2.0-or-later
// Recompiler fixes ported from the SotC native port (recompiler_toggles.h): LDS barriers for
// partial-wave and multi-wave compute workgroups, and S_WQM_B64. No window, no game.
// The switches are read once per process, so run it with their other values too:
//   out/gpu/recompiler-fixes-test [dir]
//   TLG_LDS_SMALL=0 TLG_LDS_MULTI=0 TLG_WQM=0 out/gpu/recompiler-fixes-test
// With [dir], the multi-wave compute shader is written there as SPIR-V (for spirv-val).
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <span>
#include "common/backports.h"
#include "shader_recompiler/backend/spirv/emit_spirv.h"
#include "shader_recompiler/frontend/translate/translate.h"
#include "shader_recompiler/ir/ir_emitter.h"
#include "shader_recompiler/ir/passes/ir_passes.h"
#include "shader_recompiler/profile.h"
#include "shader_recompiler/recompiler_toggles.h"
#include "shader_recompiler/runtime_info.h"
#include "video_core/amdgpu/regs_shader.h"

using namespace Shader;

static int failures = 0;

static void Check(bool ok, const char* what) {
    std::printf("%s: %s\n", ok ? "ok" : "FAIL", what);
    failures += ok ? 0 : 1;
}

static u32 Count(const IR::Block& block, IR::Opcode opcode) {
    u32 count = 0;
    for (const IR::Inst& inst : block.Instructions()) {
        count += inst.GetOpcode() == opcode ? 1 : 0;
    }
    return count;
}

struct BarrierResult {
    u32 workgroup;
    u32 subgroup;
};

// One block: an LDS write, then a read of it (another lane's slot on the guest), then a write.
static BarrierResult RunBarrierPass(u32 threads, const char* spirv_path = nullptr,
                                    u32 host_subgroup = 32) {
    Info info{};
    info.hw_stage = HwStage::Compute;
    info.sw_stage = SwStage::Compute;
    RuntimeInfo runtime{};
    runtime.Initialize(HwStage::Compute, SwStage::Compute);
    runtime.hw.cs.shared_memory_size = 256;
    runtime.hw.cs.workgroup_size = {threads, 1, 1};
    Common::ObjectPool<IR::Inst> pool;
    IR::Block block(pool);
    IR::IREmitter ir(block);
    ir.WriteShared(32, ir.Imm32(1u), ir.Imm32(0u));
    const IR::U32 value{ir.LoadShared(32, false, ir.Imm32(4u))};
    ir.WriteShared(32, value, ir.Imm32(8u));
    ir.Epilogue();
    IR::Program program(info);
    program.blocks.push_back(&block);
    program.post_order_blocks.push_back(&block);
    program.syntax_list.push_back({.data = {.block = &block},
                                   .type = IR::AbstractSyntaxNode::Type::Block});
    program.syntax_list.push_back({.type = IR::AbstractSyntaxNode::Type::Return});
    Profile profile{};
    profile.needs_lds_barriers = true;
    profile.subgroup_size = host_subgroup;
    profile.supported_spirv = 0x00010600;
    profile.support_int64 = true;
    Optimization::SharedMemoryBarrierPass(program, runtime, profile);
    const BarrierResult result{Count(block, IR::Opcode::Barrier),
                               Count(block, IR::Opcode::SubgroupBarrier)};
    if (spirv_path) {
        Optimization::CollectShaderInfoPass(program, profile); // declares the LDS types
        Backend::Bindings bindings{};
        const auto code = Backend::SPIRV::EmitSPIRV(profile, runtime, program, bindings);
        std::ofstream out(spirv_path, std::ios::binary);
        out.write(reinterpret_cast<const char*>(code.data()), code.size() * sizeof(u32));
        Check(bool(out) && !code.empty(), "multi-wave compute shader emits SPIR-V");
    }
    return result;
}

// A 64-thread compute block with one ReadFirstLane (of a lane-varying value when `varying`) and
// no ballot: lowered for wave64 (two workgroup barriers) only with TLG_RFL_ALL.
static u32 ReadFirstLaneBarriers(bool varying) {
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
    const IR::U32 source = varying ? IR::U32{ir.SharedAtomicIAdd(ir.Imm32(0u), ir.Imm32(1u), false)}
                                   : ir.GetAttributeU32(IR::Attribute::WorkgroupId, 0);
    const IR::U32 first = ir.ReadFirstLane(source);
    ir.WriteShared(32, first, ir.Imm32(0u));
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
    return Count(block, IR::Opcode::Barrier);
}

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

static u32 WqmQuadBroadcasts(Gcn::OperandField dst, Gcn::OperandField src) {
    Info info{};
    info.hw_stage = HwStage::Fragment;
    info.sw_stage = SwStage::Fragment;
    RuntimeInfo runtime{};
    runtime.Initialize(HwStage::Fragment, SwStage::Fragment);
    Profile profile{};
    Gcn::Translator translator(info, runtime, profile);
    Gcn::GcnInst inst{};
    inst.opcode = Gcn::Opcode::S_WQM_B64;
    inst.encoding = Gcn::InstEncoding::SOP1;
    inst.category = Gcn::InstCategory::ScalarALU;
    inst.length = 4;
    inst.src_count = 1;
    inst.dst_count = 1;
    inst.src[0].field = src;
    inst.dst[0].field = dst;
    Common::ObjectPool<IR::Inst> pool;
    IR::Block block(pool);
    translator.Translate(&block, 0, IR::Condition::True, std::span<const Gcn::GcnInst>{&inst, 1});
    const u32 broadcasts = Count(block, IR::Opcode::QuadBroadcast);
    if (broadcasts) {
        // One ballot reads the EXEC source (GetSrc64), one builds the quad mask.
        Check(Count(block, IR::Opcode::Ballot) == 2, "s_wqm_b64 ballots the quad mask");
        Check(Count(block, IR::Opcode::SetScc) == 1, "s_wqm_b64 sets SCC");
        Check(Count(block, IR::Opcode::SetVccLo) == 1, "s_wqm_b64 vcc, exec writes VCC");
    }
    return broadcasts;
}

// BP04: V_RCP_CLAMP_F32 / V_RSQ_CLAMP_F32 / V_LOG_CLAMP_F32 clamp to +-FLT_MAX (FPClamp32).
static u32 ClampOpClamps(Gcn::Opcode opcode) {
    Info info{};
    info.hw_stage = HwStage::Fragment;
    info.sw_stage = SwStage::Fragment;
    RuntimeInfo runtime{};
    runtime.Initialize(HwStage::Fragment, SwStage::Fragment);
    Profile profile{};
    Gcn::Translator translator(info, runtime, profile);
    Gcn::GcnInst inst{};
    inst.opcode = opcode;
    inst.encoding = Gcn::InstEncoding::VOP1;
    inst.category = Gcn::InstCategory::VectorALU;
    inst.length = 4;
    inst.src_count = 1;
    inst.dst_count = 1;
    inst.src[0].field = Gcn::OperandField::VectorGPR;
    inst.dst[0].field = Gcn::OperandField::VectorGPR;
    inst.src[0].code = 1;
    inst.dst[0].code = 2;
    Common::ObjectPool<IR::Inst> pool;
    IR::Block block(pool);
    translator.Translate(&block, 0, IR::Condition::True, std::span<const Gcn::GcnInst>{&inst, 1});
    return Count(block, IR::Opcode::FPClamp32);
}

// BP06: in a 32-thread workgroup a ReadLane of lane 32 (past the populated lanes) after a
// reduction step becomes the reduction identity; without the backport it is left alone.
static u32 ReadLanePastWorkgroup(u32 host_subgroup) {
    Info info{};
    info.hw_stage = HwStage::Compute;
    info.sw_stage = SwStage::Compute;
    RuntimeInfo runtime{};
    runtime.Initialize(HwStage::Compute, SwStage::Compute);
    runtime.hw.cs.workgroup_size = {32, 1, 1};
    Common::ObjectPool<IR::Inst> pool;
    IR::Block block(pool);
    IR::IREmitter ir(block);
    const IR::U32 x = ir.GetAttributeU32(IR::Attribute::LocalInvocationId, 0);
    const IR::U32 step{ir.IAdd(x, ir.ShuffleXor(x, ir.Imm32(16u)))};
    const IR::U32 half = ir.ReadLane(step, ir.Imm32(32u));
    ir.WriteShared(32, half, ir.Imm32(0u));
    ir.Epilogue();
    IR::Program program(info);
    program.blocks.push_back(&block);
    program.post_order_blocks.push_back(&block);
    program.syntax_list.push_back({.data = {.block = &block},
                                   .type = IR::AbstractSyntaxNode::Type::Block});
    program.syntax_list.push_back({.type = IR::AbstractSyntaxNode::Type::Return});
    Profile profile{};
    profile.subgroup_size = host_subgroup;
    Optimization::LowerWave64BallotPass(program, runtime, profile);
    return Count(block, IR::Opcode::ReadLane);
}

int main(int argc, char** argv) {
    // Exercise the new paths unless the caller chose (no game serial here: the default needs one).
    setenv("TLG_LDS_SMALL", "1", 0);
    setenv("TLG_LDS_MULTI", "1", 0);
    setenv("TLG_LDS_READLANE", "1", 0);
    setenv("TLG_LANE_DIVERGENCE", "1", 0);
    setenv("TLG_BP06", "1", 0); // default off: exercise it unless the caller chose
    const bool small = Toggles::SmallGroupLdsBarriers();
    const bool multi = Toggles::MultiWaveLdsBarriers();
    const bool wqm = Toggles::WholeQuadMode();
    std::printf("TLG_LDS_SMALL=%d TLG_LDS_MULTI=%d TLG_WQM=%d\n", small, multi, wqm);

    const BarrierResult wave = RunBarrierPass(64);
    Check(wave.workgroup == 3 && wave.subgroup == 0, "64 threads: workgroup barriers (unchanged)");
    const BarrierResult partial = RunBarrierPass(16);
    Check(partial.workgroup == (small ? 3u : 0u) && partial.subgroup == 0,
          "16 threads: workgroup barriers with TLG_LDS_SMALL");
    std::string spirv;
    if (argc > 1) {
        std::filesystem::create_directories(argv[1]);
        spirv = (std::filesystem::path{argv[1]} / "lds-multi-wave.spv").string();
    }
    // 32-wide host: every guest wave spans two host subgroups, so workgroup barriers.
    const BarrierResult several = RunBarrierPass(256, spirv.empty() ? nullptr : spirv.c_str());
    Check(several.workgroup == (multi ? 3u : 0u) && several.subgroup == 0,
          "256 threads, 32-wide host: workgroup barriers with TLG_LDS_MULTI");
    const BarrierResult odd = RunBarrierPass(96);
    Check(odd.workgroup == (multi ? 3u : 0u) && odd.subgroup == 0,
          "96 threads, 32-wide host: workgroup barriers with TLG_LDS_MULTI (was none)");
    const BarrierResult small48 = RunBarrierPass(48);
    Check(small48.workgroup == (small ? 3u : 0u) && small48.subgroup == 0,
          "48 threads: workgroup barriers with TLG_LDS_SMALL");
    // 64-wide host: each guest wave (the partial last one too) is one host subgroup.
    const BarrierResult wide = RunBarrierPass(256, nullptr, 64);
    Check(wide.workgroup == 0 && wide.subgroup == (multi ? 3u : 0u),
          "256 threads, 64-wide host: subgroup barriers with TLG_LDS_MULTI");
    const BarrierResult wide_odd = RunBarrierPass(96, nullptr, 64);
    Check(wide_odd.workgroup == 0 && wide_odd.subgroup == (multi ? 3u : 0u),
          "96 threads, 64-wide host: subgroup barriers with TLG_LDS_MULTI");
    const BarrierResult unknown = RunBarrierPass(192, nullptr, 0);
    Check(unknown.workgroup == (multi ? 3u : 0u) && unknown.subgroup == 0,
          "192 threads, unknown host subgroup: workgroup barriers");

    {
        Common::ObjectPool<IR::Inst> pool;
        IR::Block block(pool);
        IR::IREmitter ir(block);
        const IR::U32 tid = ir.GetAttributeU32(IR::Attribute::LocalInvocationId, 0);
        const IR::U32 index = ir.GetAttributeU32(IR::Attribute::LocalInvocationIndex);
        const IR::U32 lt_mask = ir.GetAttributeU32(IR::Attribute::SubgroupLtMask, 0);
        const IR::U32 wg = ir.GetAttributeU32(IR::Attribute::WorkgroupId, 0);
        const IR::U32 old = ir.SharedAtomicIAdd(ir.Imm32(0u), ir.Imm32(1u), false);
        const bool lane = Toggles::LaneDivergence();
        Check(Optimization::IsLaneVaryingSource(*tid.Inst()), "LocalInvocationId varies per lane");
        Check(Optimization::IsLaneVaryingSource(*index.Inst()) == lane,
              "LocalInvocationIndex varies per lane with TLG_LANE_DIVERGENCE");
        Check(Optimization::IsLaneVaryingSource(*lt_mask.Inst()) == lane,
              "the mbcnt lane mask varies per lane with TLG_LANE_DIVERGENCE");
        Check(Optimization::IsLaneVaryingSource(*old.Inst()) == lane,
              "an atomic's old value varies per lane with TLG_LANE_DIVERGENCE");
        Check(!Optimization::IsLaneVaryingSource(*wg.Inst()), "WorkgroupId is uniform");
    }

    {
        // Binary info: found right after the code, rejected when its length runs past it.
        alignas(256) static u32 code[64]{};
        code[0] = 0xBF810000; // s_endpgm
        auto* info = reinterpret_cast<AmdGpu::BinaryInfo*>(&code[8]);
        std::memcpy(info->signature.data(), AmdGpu::BinaryInfo::signature_ref.data(),
                    info->signature.size());
        info->length = 8 * sizeof(u32);
        info->shader_hash = 0x1234;
        Check(AmdGpu::FindBinaryInfo(code) == info, "binary info right after the code is found");
        info->length = 40 * sizeof(u32);
        Check(AmdGpu::FindBinaryInfo(code) == nullptr,
              "binary info whose code would run past it is rejected");
        info->length = 0;
        Check(AmdGpu::FindBinaryInfo(code) == nullptr, "binary info with no code is rejected");
    }

    const bool rfl_all = Toggles::ReadFirstLaneAll();
    Check(ReadFirstLaneBarriers(true) == (rfl_all ? 2u : 0u),
          "ReadFirstLane of a lane-varying value is lowered for wave64 with TLG_RFL_ALL");
    Check(ReadFirstLaneBarriers(false) == 0, "ReadFirstLane of a uniform value is left alone");
    Check(BallotReadsSlotPair(), "wave64 ballot on a 32-wide host: both halves read the slot pair");

    Check(WqmQuadBroadcasts(Gcn::OperandField::VccLo, Gcn::OperandField::ExecLo) ==
              (wqm ? 4u : 0u),
          "s_wqm_b64 vcc, exec grows the mask to quads with TLG_WQM");
    Check(WqmQuadBroadcasts(Gcn::OperandField::ExecLo, Gcn::OperandField::ExecLo) == 0,
          "s_wqm_b64 exec, exec stays a no-op");
    {
        using Common::Backports::Id;
        const bool bp04 = Common::Backports::On(Id::ClampOpsPlain);
        const u32 want = bp04 ? 1u : 0u;
        Check(ClampOpClamps(Gcn::Opcode::V_RCP_CLAMP_F32) == want, "BP04: v_rcp_clamp_f32 clamps");
        Check(ClampOpClamps(Gcn::Opcode::V_RSQ_CLAMP_F32) == want, "BP04: v_rsq_clamp_f32 clamps");
        Check(ClampOpClamps(Gcn::Opcode::V_LOG_CLAMP_F32) == want, "BP04: v_log_clamp_f32 clamps");
        Check(ClampOpClamps(Gcn::Opcode::V_RSQ_F32) == 0, "BP04: v_rsq_f32 stays unclamped");
        const bool bp06 = Common::Backports::On(Id::MaximalReconvergence);
        // 32-lane host: the read is replaced with BP06; 64-lane host: likewise (it used to return
        // before touching the shader).
        Check(ReadLanePastWorkgroup(32) == (bp06 ? 0u : 1u),
              "BP06: readlane 32 in a 32-thread workgroup, 32-wide host");
        Check(ReadLanePastWorkgroup(64) == (bp06 ? 0u : 1u),
              "BP06: readlane 32 in a 32-thread workgroup, 64-wide host");
    }
    Check(Toggles::ShaderBinarySalt() ==
              ((small ? 1u : 0u) | (multi ? 2u : 0u) | (Toggles::LdsUniformReadLane() ? 4u : 0u) |
               (wqm ? 0u : 8u) | (Toggles::LaneDivergence() ? 16u : 0u) |
               (Toggles::ReadFirstLaneAll() ? 32u : 0u)),
          "shader cache salt follows the switches");
    {
        // Backport switches: tlg.ini choices apply unless the environment sets the entry; only the
        // live ones can be switched while the game runs.
        using Common::Backports::Id;
        unsetenv("TLG_BP10");
        Common::Backports::ApplyConfig(static_cast<std::size_t>(Id::VertexIndexBarriers), false);
        Check(!Common::Backports::On(Id::VertexIndexBarriers), "ini choice turns BP10 off");
        setenv("TLG_BP10", "1", 1);
        Common::Backports::ApplyConfig(static_cast<std::size_t>(Id::VertexIndexBarriers), true);
        Check(!Common::Backports::On(Id::VertexIndexBarriers), "environment variable wins over the ini (state kept)");
        unsetenv("TLG_BP10");
        Check(Common::Backports::SetLive(Id::VertexIndexBarriers, true) &&
                  Common::Backports::On(Id::VertexIndexBarriers),
              "a live backport can be switched in game");
        const bool before = Common::Backports::On(Id::ClampOpsPlain);
        Check(!Common::Backports::SetLive(Id::ClampOpsPlain, !before) &&
                  Common::Backports::On(Id::ClampOpsPlain) == before,
              "a shader-changing backport refuses a live switch");
    }
    std::printf("%s\n", failures ? "recompiler fixes: FAILED" : "recompiler fixes: all ok");
    return failures ? 1 : 0;
}
