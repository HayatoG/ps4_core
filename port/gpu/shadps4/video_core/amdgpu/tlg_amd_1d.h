// SPDX-License-Identifier: GPL-2.0-or-later
// tlg: diagnostics and switches for 1D images on AMD's Windows driver (iss_pc #16, #31, #49,
// #53). With BC decoding (tlg_bc_tiny.h) the RX 580 still writes through a null pointer at
// amdvlk64.dll+0xebbbb right after the 512x1, 32-layer, 10-mip R8G8B8A8Srgb 1D image is made,
// so the cause is the 1D image itself, not the compression.
//   TLG_IMG_TRACE=1/0: print (and flush) every step for 1D images: create, view, upload record,
//                      upload execution. Default: on for AMD's proprietary driver.
//   TLG_1D_NO_STORAGE=1: create 1D images without storage usage.
//   TLG_1D_NO_MUTABLE=1: create 1D images without the mutable-format/extended-usage flags.
#pragma once

#include <atomic>
#include <cstdio>

namespace AmdGpu {

inline std::atomic<bool> img_trace{false};
inline std::atomic<bool> img_1d_no_storage{false};
inline std::atomic<bool> img_1d_no_mutable{false};

// The trace is for the crash that follows the first 1D image, so it stops after a few thousand lines:
// a 1D image that is transitioned every frame filled the log (and the launcher's window) with
// "GPU trace: Transit". TLG_IMG_TRACE_LINES=N changes the limit, 0 removes it.
inline std::atomic<unsigned> img_trace_lines{0};
inline std::atomic<unsigned> img_trace_limit{2000};

inline bool ImgTrace() {
    return img_trace.load(std::memory_order_relaxed) &&
           img_trace_lines.load(std::memory_order_relaxed) < img_trace_limit.load(std::memory_order_relaxed);
}

} // namespace AmdGpu

// Printed and flushed at once: the driver may crash on the very next call.
#define TLG_IMG_TRACE(...)                                                                         \
    do {                                                                                           \
        if (::AmdGpu::ImgTrace()) {                                                                \
            const unsigned trace_n = ::AmdGpu::img_trace_lines.fetch_add(1) + 1;                   \
            std::printf("GPU trace: " __VA_ARGS__);                                                \
            if (trace_n == ::AmdGpu::img_trace_limit.load()) {                                     \
                std::printf("GPU trace: stopped after %u lines (TLG_IMG_TRACE_LINES=0 keeps it)\n", \
                            trace_n);                                                              \
            }                                                                                      \
            std::fflush(stdout);                                                                   \
        }                                                                                          \
    } while (0)
