// SPDX-License-Identifier: GPL-2.0-or-later
// tlg: tiny block-compressed textures decoded on the CPU (TLG_BC_TINY / ISS_BC_TINY).
//
// AMD's Windows driver (amdvlk64.dll, RX 580) writes through a null pointer at
// amdvlk64.dll+0xebbbb right after the game's first BC1 1D texture (512x1, iss_pc #16, #31,
// #49), whether that image is created 1D (unsupported) or 2D of height 1 (TLG_BLOCK1D, #36).
// With this on, BC1-BC5 textures that are 1D or narrower or shorter than one 4x4 block become
// plain RGBA8 / R8 / RG8 images (sRGB and snorm kept) filled from a CPU decode of guest memory
// (texture_cache/tlg_bc_decode.h): the driver never sees a block-compressed image of that
// shape. 1D ones stay 1D (no TLG_BLOCK1D promotion), which every driver supports.
//
// Default: on for AMD's proprietary driver only; TLG_BC_TINY=1 or 0 decides otherwise.
// vk_instance.cpp sets it once, before any image or shader exists, and the shader profile
// carries it (it changes which 1D textures the shaders sample as 2D).

#pragma once

#include <atomic>

namespace AmdGpu {

inline std::atomic<bool> bc_tiny_decode{false};

inline bool BcTinyDecode() {
    return bc_tiny_decode.load(std::memory_order_relaxed);
}

} // namespace AmdGpu
