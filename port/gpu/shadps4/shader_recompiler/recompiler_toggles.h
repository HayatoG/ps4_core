// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// tlg: recompiler fixes ported from the Shadow of the Colossus native port
// (github.com/AjncSystem/ShadowOfTheColossus-PC, MIT/GPL-2.0, commits 6f0b3e56, debb0c49,
// e3227814 and its vendored shared_memory_barrier_pass.cpp at 70bb4385) and the env switches
// that turn them on or off for A/B tests. Each switch is read once. The ISS_/GR2_ names reach
// here as TLG_ (the probe aliases the port prefix at start-up).
//
// Shader loop budget, from the Shadow of the Colossus native port
// (github.com/AjncSystem/ShadowOfTheColossus-PC, GPL-2.0, commits 3cfed48d and 3bc854ce): every
// structured loop of a recompiled shader counts its iterations and is left once it runs past a
// budget. A guest loop that terminates on the PS4 but not on the host (stale or garbage data
// from emulation, EXEC/wave64 differences, NaN compares) would otherwise spin until the OS GPU
// watchdog resets the device ("Device lost during submit", Windows TDR). Real content stays far
// below the budgets, so a cut loop only spoils one frame instead of ending the session.

#pragma once

#include <cstdint>
#include <cstdlib>
#include <initializer_list>

namespace Shader::Toggles {

namespace Detail {
/// TLG_<name>=0/1 when set, otherwise the default (only evaluated then).
inline bool EnvFlag(const char* name, bool (*fallback)()) {
    const char* v = std::getenv(name);
    return v && *v ? std::atoi(v) != 0 : fallback();
}

inline bool AlwaysOn() {
    return true;
}

/// Off unless the game's profile (port.env / profile.ini) or the player sets the switch: the LDS
/// barrier fixes were aimed at inFAMOUS' GPU particle smoke (iss_pc #7), whose profile turns them
/// on (TLG_LDS_SMALL/MULTI/READLANE=1, TLG_LANE_DIVERGENCE=1). The core names no game.
inline bool ProfileDefault() {
    return false;
}

/// TLG_<name>=<number> (decimal, 0x hex or 0 octal) when set, otherwise the default.
inline std::uint32_t EnvU32(const char* name, std::uint32_t fallback) {
    const char* v = std::getenv(name);
    if (!v || !*v) {
        return fallback;
    }
    char* end = nullptr;
    const unsigned long long value = std::strtoull(v, &end, 0);
    if (end == v) {
        return fallback;
    }
    return value > 0xffffffffull ? 0xffffffffu : static_cast<std::uint32_t>(value);
}
} // namespace Detail

/// Workgroups smaller than one GCN wave (<64 threads) get LDS barriers like a 64-thread one.
/// TLG_LDS_SMALL=0/1; default off (on in the inFAMOUS profile).
inline bool SmallGroupLdsBarriers() {
    static const bool enabled = Detail::EnvFlag("TLG_LDS_SMALL", &Detail::ProfileDefault);
    return enabled;
}

/// Workgroups larger than one wave (any size above 64, a multiple of 64 or not: 96, 192, 256,
/// ...) get barriers where the guest relies on lockstep inside one wave. On 32-wide hardware
/// every 64-lane guest wave spans two host subgroups, so these are workgroup barriers; only
/// when each guest wave fits in one host subgroup (64-wide hardware) is a subgroup barrier
/// enough. TLG_LDS_MULTI=0/1; default off (on in the inFAMOUS profile).
inline bool MultiWaveLdsBarriers() {
    static const bool enabled = Detail::EnvFlag("TLG_LDS_MULTI", &Detail::ProfileDefault);
    return enabled;
}

/// The LDS barrier pass treats a ReadLane with a constant lane in uniform flow as uniform when
/// it decides whether a branch is divergent (one-wave workgroups only). From Pink-shadPS4
/// (luizgustavs) via SotC. TLG_LDS_READLANE=0/1; default off (on in the inFAMOUS profile).
inline bool LdsUniformReadLane() {
    static const bool enabled = Detail::EnvFlag("TLG_LDS_READLANE", &Detail::ProfileDefault);
    return enabled;
}

/// S_WQM_B64 writes the source mask grown to whole quads (it was a no-op). TLG_WQM=0 restores
/// the no-op; default on for every game (correctness fix).
inline bool WholeQuadMode() {
    static const bool enabled = Detail::EnvFlag("TLG_WQM", &Detail::AlwaysOn);
    return enabled;
}

/// Branch divergence (LDS barrier pass and wave64 lowering) also counts the lane-varying
/// sources other than LocalInvocationId: LocalInvocationIndex, LaneId, the lane masks behind
/// v_mbcnt (SubgroupLtMask) and the values returned by atomics. Without it the append pattern's
/// "if (mbcnt == 0) atomic" looked uniform, so barriers (the wave64 exchanges included) landed
/// inside a branch only one lane takes, which Vulkan leaves undefined. TLG_LANE_DIVERGENCE=0
/// restores the old test; default off (on in the inFAMOUS profile).
inline bool LaneDivergence() {
    static const bool enabled = Detail::EnvFlag("TLG_LANE_DIVERGENCE", &Detail::ProfileDefault);
    return enabled;
}

/// Lowers v_readfirstlane in every compute shader wider than a 32-lane host wave (not only in
/// those that also use ballots or v_mbcnt): the two host halves of a wave64 swap the value
/// through LDS so both read the first active lane of the whole wave, in uniform flow, unless the
/// value is provably the same in every lane. TLG_RFL_ALL=0/1; default off (experiment: adds two
/// workgroup barriers per lowered instruction).
inline bool ReadFirstLaneAll() {
    static const bool enabled = Detail::EnvFlag("TLG_RFL_ALL", [] { return false; });
    return enabled;
}


inline constexpr std::uint32_t DefaultLoopBudget = 1u << 18;
inline constexpr std::uint32_t DefaultPerLoopBudget = 8192u;

/// Total loop iterations one shader invocation may run across all its (nested) loops.
/// TLG_LOOP_LIMIT=<n>; 0 turns every loop budget off (this one and the per-loop one).
inline std::uint32_t LoopBudget() {
    static const std::uint32_t budget = Detail::EnvU32("TLG_LOOP_LIMIT", DefaultLoopBudget);
    return budget;
}

/// Iterations of one loop, counted again from zero each time the loop is entered: a runaway
/// loop stops early, while a shader that runs many short loops one after another only meets
/// the invocation budget. TLG_LOOP_LIMIT_PER_LOOP=<n>; 0 turns only this one off.
inline std::uint32_t PerLoopBudget() {
    static const std::uint32_t budget =
        LoopBudget() == 0 ? 0u : Detail::EnvU32("TLG_LOOP_LIMIT_PER_LOOP", DefaultPerLoopBudget);
    return budget;
}

/// Shader binary cache salt of the loop budgets (vk_pipeline_serialization.cpp): 0 with the
/// defaults; otherwise a 16-bit hash of the budgets (never 0).
inline std::uint32_t LoopBudgetSalt() {
    const std::uint32_t total = LoopBudget();
    const std::uint32_t own = PerLoopBudget();
    if (total == DefaultLoopBudget && own == DefaultPerLoopBudget) {
        return 0u;
    }
    std::uint32_t h = 2166136261u; // FNV-1a over both values
    for (const std::uint32_t value : {total, own}) {
        for (int shift = 0; shift < 32; shift += 8) {
            h = (h ^ ((value >> shift) & 0xffu)) * 16777619u;
        }
    }
    h = (h ^ (h >> 16)) & 0xffffu;
    return h == 0 ? 1u : h;
}

/// Bits for the shader binary cache version: shaders recompiled under other switches must not
/// be reused (the cache key only hashes the recompiler sources).
inline std::uint32_t ShaderBinarySalt() {
    return (SmallGroupLdsBarriers() ? 1u : 0u) | (MultiWaveLdsBarriers() ? 2u : 0u) |
           (LdsUniformReadLane() ? 4u : 0u) | (WholeQuadMode() ? 0u : 8u) |
           (LaneDivergence() ? 16u : 0u) | (ReadFirstLaneAll() ? 32u : 0u);
}

/// One line per switch with its state (shown once, when the GPU starts). Takes the logger as a
/// callback so this header stays free of the logging includes.
template <typename Log>
inline void LogStates(Log&& log) {
    log("TLG_LDS_SMALL", SmallGroupLdsBarriers(), "LDS barriers in workgroups under 64 threads");
    log("TLG_LDS_MULTI", MultiWaveLdsBarriers(),
        "LDS barriers in workgroups over 64 threads (workgroup scope on 32-wide GPUs)");
    log("TLG_LDS_READLANE", LdsUniformReadLane(), "uniform constant-lane ReadLane in LDS barriers");
    log("TLG_WQM", WholeQuadMode(), "S_WQM_B64 grows the mask to quads");
    log("TLG_LANE_DIVERGENCE", LaneDivergence(),
        "mbcnt masks, lane ids and atomic results make a branch divergent");
    log("TLG_RFL_ALL", ReadFirstLaneAll(), "wave64 ReadFirstLane lowered in every wide compute shader");
}

} // namespace Shader::Toggles
