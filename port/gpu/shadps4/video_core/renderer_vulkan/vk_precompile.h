// SPDX-License-Identifier: GPL-2.0-or-later
// tlg: first-run shader compilation. A job file lists draws and dispatches as the pipeline
// cache sees them (registers, shader code, flattened user data, vertex buffer descriptors);
// before the game starts, the pipeline cache compiles each one exactly as if the game had drawn
// it, so the shaders and pipelines are ready (and saved to the shader cache) from the first
// frame. The job file is built on the player's machine from the player's own game files; a
// development capture (TLG_PRECOMPILE_CAPTURE=<file>) writes the same records while playing,
// to check the precompiler and the job builder against what the game really draws.
#pragma once

#include <filesystem>
#include <functional>
#include <vector>
#include "common/types.h"

namespace AmdGpu {
union Regs;
struct ComputeProgram;
} // namespace AmdGpu

namespace Vulkan {

struct PipelineSelection;
class PipelineCache;

namespace Precompile {

/// Job file layout (little-endian), shared with port/tools/xpps (the job builder):
///   "ISSPJOB1", u32 version
///   record: u32 kind (0 graphics, 1 compute)
///           u32 n, then n × {u32 dword index, u32 value}: the nonzero register dwords
///             (graphics: AmdGpu::Regs::reg_array; compute: AmdGpu::ComputeProgram)
///           u32 stages, then per stage:
///             u32 sw stage, u32 hw stage, u64 program hash
///             u32 bytes, code from its first instruction to the end of its OrbShdr block
///             u32 user_data[16]
///             u32 n, u32 flattened user data[n]
///             u32 fetch shader bytes, the code (0: none), u32 its user data register
///             u32 n, then n × {u32 user data register, u32 dword offset, u32 V#[4]}
inline constexpr char FileMagic[8] = {'I', 'S', 'S', 'P', 'J', 'O', 'B', '1'};
inline constexpr u32 FileVersion = 1;

struct VertexSharp {
    u32 sgpr_base;
    u32 dword_offset;
    std::array<u32, 4> sharp;
};

struct Stage {
    u32 sw_stage;
    u32 hw_stage;
    u64 hash;
    std::vector<u8> code; ///< up to the end of the OrbShdr block (FindBinaryInfo needs it)
    std::array<u32, 16> user_data;
    std::vector<u32> flat;
    std::vector<u8> fetch_code;
    u32 fetch_sgpr = 0;
    std::vector<VertexSharp> vertex_sharps;
};

struct Record {
    u32 kind; ///< 0 graphics, 1 compute
    std::vector<std::pair<u32, u32>> regs;
    std::vector<Stage> stages;
};

/// Reads a job file; false when it is missing or malformed (the reason goes to stdout).
bool ReadJobs(const std::filesystem::path& path, std::vector<Record>& records);

/// TLG_PRECOMPILE_CAPTURE: appends the draw/dispatch of every new pipeline (GPU thread).
bool CaptureEnabled();
void CaptureGraphics(const PipelineSelection& sel);
void CaptureCompute(const PipelineSelection& sel, const AmdGpu::ComputeProgram& cs);

struct Result {
    u32 records = 0, graphics = 0, compute = 0, failed = 0, skipped = 0;
};

/// The job file of this game: TLG_PRECOMPILE_JOBS, else <gpu user>/cache/<serial>-jobs.bin
/// (the launcher writes it from the game files; it outlives cache rebuilds).
std::filesystem::path JobFile();
/// Files in the shader cache folder (Clear removes them with the cache): `done` says which
/// job file was compiled into this cache; `progress` the last record started, so a crash in
/// one record resumes after it instead of failing again at every start.
inline constexpr const char* DoneFile = "precompiled.txt";
inline constexpr const char* ProgressFile = "precompile-progress.txt";

} // namespace Precompile
} // namespace Vulkan
