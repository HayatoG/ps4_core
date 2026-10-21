// SPDX-License-Identifier: GPL-2.0-or-later
// tlg: helper thread sizing. Counts follow the hardware threads this process may run on
// (the affinity mask, so `taskset` can emulate a Steam Deck), and speculative helpers run as
// SCHED_IDLE: they use cores the game leaves idle and never take time from its threads.

#pragma once

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>
#ifdef _WIN32
#include <bit>
#include <windows.h>
#else
#include <sched.h>
#include <sys/resource.h>
#include <unistd.h>
#endif

namespace TlgThreads {
#ifdef _WIN32
/// Hardware threads available to the process (its affinity mask).
inline unsigned Available() {
    DWORD_PTR process = 0, system = 0;
    if (GetProcessAffinityMask(GetCurrentProcess(), &process, &system) && process) {
        return std::max(1, std::popcount(static_cast<unsigned long long>(process)));
    }
    return std::max(1u, std::thread::hardware_concurrency());
}

/// Physical cores (SMT siblings counted once) the process may run on, from the processor
/// topology; the hardware threads when that cannot be read.
inline unsigned PhysicalCores() {
    DWORD_PTR process = 0, system = 0;
    const bool has_affinity =
        GetProcessAffinityMask(GetCurrentProcess(), &process, &system) && process;
    DWORD length = 0;
    GetLogicalProcessorInformation(nullptr, &length);
    if (length == 0) {
        return Available();
    }
    std::vector<SYSTEM_LOGICAL_PROCESSOR_INFORMATION> info(
        length / sizeof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION) + 1);
    if (!GetLogicalProcessorInformation(info.data(), &length)) {
        return Available();
    }
    const size_t count = length / sizeof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION);
    unsigned cores = 0;
    for (size_t i = 0; i < count; ++i) {
        if (info[i].Relationship != RelationProcessorCore) {
            continue;
        }
        if (has_affinity && (info[i].ProcessorMask & process) == 0) {
            continue;
        }
        ++cores;
    }
    return std::max(1u, cores);
}

/// The calling thread only runs when cores are otherwise idle (lowest Windows priority).
inline void MakeBackground() {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_IDLE);
}
#else

/// Hardware threads available to the process.
inline unsigned Available() {
    cpu_set_t set;
    CPU_ZERO(&set);
    if (sched_getaffinity(0, sizeof(set), &set) == 0) {
        return std::max(1, CPU_COUNT(&set));
    }
    return std::max(1u, std::thread::hardware_concurrency());
}

/// Physical cores (SMT siblings counted once) the process may run on: every allowed CPU's
/// thread_siblings_list starts with the number of its physical core. The hardware threads when
/// sysfs cannot be read.
inline unsigned PhysicalCores() {
    cpu_set_t set;
    CPU_ZERO(&set);
    if (sched_getaffinity(0, sizeof(set), &set) != 0) {
        return Available();
    }
    std::vector<int> cores;
    for (int cpu = 0; cpu < CPU_SETSIZE; ++cpu) {
        if (!CPU_ISSET(cpu, &set)) {
            continue;
        }
        char path[128];
        std::snprintf(path, sizeof(path),
                      "/sys/devices/system/cpu/cpu%d/topology/thread_siblings_list", cpu);
        std::FILE* file = std::fopen(path, "r");
        if (!file) {
            return Available();
        }
        int core = -1;
        const int read = std::fscanf(file, "%d", &core);
        std::fclose(file);
        if (read != 1) {
            return Available();
        }
        if (std::find(cores.begin(), cores.end(), core) == cores.end()) {
            cores.push_back(core);
        }
    }
    return cores.empty() ? Available() : static_cast<unsigned>(cores.size());
}

/// The calling thread only runs on otherwise idle cores (falls back to the lowest nice level).
inline void MakeBackground() {
    sched_param param{};
    if (sched_setscheduler(0, SCHED_IDLE, &param) != 0) {
        setpriority(PRIO_PROCESS, static_cast<id_t>(gettid()), 19);
    }
}
#endif

/// TLG_PHYS_CORES=1 (default off): the counts the spin budgets and the helper threads of the
/// draw pipeline are sized with come from the physical cores instead of the hardware threads,
/// so a 6 core / 12 thread machine spins and starts helpers like a 6 thread one. Logged once.
inline bool UsePhysicalCores() {
    static const bool on = [] {
        const char* env = std::getenv("TLG_PHYS_CORES");
        const bool enabled = env && env[0] == '1';
        if (enabled) {
            std::printf("GPU: helper sizing by physical cores (TLG_PHYS_CORES=1)\n");
        }
        return enabled;
    }();
    return on;
}

/// What the long-spin thresholds (DrawPipe, the recorder) and DefaultWorkerCount count:
/// TlgThreads::PhysicalCores() with TLG_PHYS_CORES=1, the hardware threads otherwise. Read
/// once (PhysicalCores() reads sysfs).
inline unsigned SizingCount() {
    static const unsigned count = UsePhysicalCores() ? PhysicalCores() : Available();
    return count;
}

} // namespace TlgThreads
