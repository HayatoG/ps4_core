// SPDX-License-Identifier: GPL-2.0-or-later
// Shader loop budgets (recompiler_toggles.h): every structured loop counts its iterations into
// an invocation-wide counter and its own counter, and takes the back edge only while both are
// under budget. No window, no game, no Vulkan device. The switches are read once per process,
// so run it with their other values too:
//   out/gpu/loop-budget-test [dir]
//   TLG_LOOP_LIMIT_PER_LOOP=0 out/gpu/loop-budget-test
//   TLG_LOOP_LIMIT=0 out/gpu/loop-budget-test
// With [dir], the shaders are written there as SPIR-V and checked with spirv-val when it is
// installed.
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>
#include "shader_recompiler/backend/spirv/emit_spirv.h"
#include "shader_recompiler/ir/ir_emitter.h"
#include "shader_recompiler/ir/passes/ir_passes.h"
#include "shader_recompiler/profile.h"
#include "shader_recompiler/recompiler_toggles.h"
#include "shader_recompiler/runtime_info.h"

using namespace Shader;

static int failures = 0;

static void Check(bool ok, const std::string& what) {
    std::printf("%s: %s\n", ok ? "ok" : "FAIL", what.c_str());
    failures += ok ? 0 : 1;
}

struct SpirvCounts {
    u32 loop_merges = 0;
    u32 ult = 0;          // OpULessThan
    u32 private_vars = 0; // OpVariable ... Private
    bool has_per_loop_constant = false;
    bool has_total_constant = false;
};

static SpirvCounts Scan(const std::vector<u32>& code) {
    SpirvCounts c{};
    for (size_t i = 5; i < code.size();) {
        const u32 words = code[i] >> 16;
        const u32 op = code[i] & 0xffff;
        if (words == 0) {
            break;
        }
        if (op == 246) { // OpLoopMerge
            ++c.loop_merges;
        } else if (op == 176) { // OpULessThan
            ++c.ult;
        } else if (op == 59 && words >= 4 && code[i + 3] == 6) { // OpVariable, Private
            ++c.private_vars;
        } else if (op == 43 && words == 4) { // OpConstant (32-bit)
            c.has_per_loop_constant |= code[i + 3] == Toggles::PerLoopBudget();
            c.has_total_constant |= code[i + 3] == Toggles::LoopBudget();
        }
        i += words;
    }
    return c;
}

// The syntax list the structurizer builds for a loop: Block(header), Loop, body..., Block(cont),
// Repeat, Block(merge). `depth` loops nested in one another, each `while (true)`: without a
// budget the shader would never end.
struct LoopShader {
    Common::ObjectPool<IR::Inst> pool;
    std::vector<std::unique_ptr<IR::Block>> storage;
    Info info{};
    RuntimeInfo runtime{};

    IR::Block* NewBlock() {
        storage.push_back(std::make_unique<IR::Block>(pool));
        return storage.back().get();
    }

    std::vector<u32> Emit(u32 depth) {
        info.hw_stage = HwStage::Compute;
        info.sw_stage = SwStage::Compute;
        runtime.Initialize(HwStage::Compute, SwStage::Compute);
        runtime.hw.cs.workgroup_size = {64, 1, 1};
        IR::Program program(info);
        const auto block_node = [&](IR::Block* block) {
            program.blocks.push_back(block);
            program.syntax_list.push_back(
                {.data = {.block = block}, .type = IR::AbstractSyntaxNode::Type::Block});
        };
        block_node(NewBlock()); // entry: a loop header may not be the first block
        const auto emit_loop = [&](auto&& self, u32 level) -> void {
            IR::Block* header = NewBlock();
            IR::Block* body = NewBlock();
            IR::Block* cont = NewBlock();
            IR::Block* merge = NewBlock();
            block_node(header);
            program.syntax_list.push_back(
                {.data = {.loop = {.body = body, .continue_block = cont, .merge = merge}},
                 .type = IR::AbstractSyntaxNode::Type::Loop});
            block_node(body);
            if (level > 1) {
                self(self, level - 1);
            }
            block_node(cont);
            program.syntax_list.push_back(
                {.data = {.repeat = {.cond = IR::U1{IR::Value{true}}, .loop_header = header,
                                     .merge = merge}},
                 .type = IR::AbstractSyntaxNode::Type::Repeat});
            block_node(merge);
        };
        emit_loop(emit_loop, depth);
        program.syntax_list.push_back({.type = IR::AbstractSyntaxNode::Type::Return});
        for (auto it = program.blocks.rbegin(); it != program.blocks.rend(); ++it) {
            program.post_order_blocks.push_back(*it);
        }
        Profile profile{};
        profile.subgroup_size = 32;
        profile.supported_spirv = 0x00010600;
        profile.support_int64 = true;
        Optimization::CollectShaderInfoPass(program, profile);
        Backend::Bindings bindings{};
        return Backend::SPIRV::EmitSPIRV(profile, runtime, program, bindings);
    }
};

static bool Validate(const std::filesystem::path& file) {
    if (std::system("command -v spirv-val >/dev/null 2>&1") != 0) {
        std::printf("skip: spirv-val not installed (%s)\n", file.string().c_str());
        return true;
    }
    const std::string cmd = "spirv-val --target-env vulkan1.3 \"" + file.string() + "\"";
    return std::system(cmd.c_str()) == 0;
}

int main(int argc, char** argv) {
    const u32 total = Toggles::LoopBudget();
    const u32 own = Toggles::PerLoopBudget();
    std::printf("TLG_LOOP_LIMIT=%u TLG_LOOP_LIMIT_PER_LOOP=%u\n", total, own);
    if (!std::getenv("TLG_LOOP_LIMIT") && !std::getenv("TLG_LOOP_LIMIT_PER_LOOP")) {
        Check(total == (1u << 18) && own == 8192u, "defaults: 1 << 18 per invocation, 8192 per loop");
        Check(Toggles::LoopBudgetSalt() == 0, "defaults keep the plain shader cache version");
    } else {
        Check(Toggles::LoopBudgetSalt() != 0, "other budgets get their own shader cache version");
    }
    std::filesystem::path dir;
    if (argc > 1) {
        dir = argv[1];
        std::filesystem::create_directories(dir);
    }
    for (const u32 depth : {1u, 2u, 3u}) {
        LoopShader shader;
        const std::vector<u32> code = shader.Emit(depth);
        const std::string name = std::to_string(depth) + " nested loop(s)";
        Check(!code.empty(), name + ": emits SPIR-V");
        const SpirvCounts c = Scan(code);
        Check(c.loop_merges == depth, name + ": one OpLoopMerge per loop");
        const u32 checks = total == 0 ? 0 : depth * (own != 0 ? 2u : 1u);
        Check(c.ult == checks, name + ": budget compares on the back edges");
        const u32 vars = total == 0 ? 0 : 1 + (own != 0 ? depth : 0);
        Check(c.private_vars == vars, name + ": one counter per loop plus the invocation one");
        if (total != 0) {
            Check(c.has_total_constant, name + ": invocation budget constant");
        }
        if (own != 0) {
            Check(c.has_per_loop_constant, name + ": per-loop budget constant");
        }
        if (!dir.empty()) {
            const auto file = dir / ("loop-budget-" + std::to_string(depth) + ".spv");
            std::ofstream out(file, std::ios::binary);
            out.write(reinterpret_cast<const char*>(code.data()), code.size() * sizeof(u32));
            out.close();
            Check(bool(out) && Validate(file), name + ": spirv-val");
        }
    }
    std::printf("%s\n", failures ? "loop budget: FAILED" : "loop budget: all ok");
    return failures ? 1 : 0;
}
