// SPDX-License-Identifier: GPL-2.0-or-later
// TLG_BC_TINY (video_core/amdgpu/tlg_bc_tiny.h): the CPU BC1-BC5 decoder and the micro-tile
// addressing in video_core/texture_cache/tlg_bc_decode.h, against hand-built blocks with known
// texels. No game or Vulkan device required.
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <set>

#include "video_core/texture_cache/tlg_bc_decode.h"

using namespace VideoCore::BcDecode;

static uint16_t Rgb565(uint32_t r, uint32_t g, uint32_t b) {
    return uint16_t((r << 11) | (g << 5) | b);
}

static void Put16(uint8_t* p, uint16_t v) {
    p[0] = uint8_t(v);
    p[1] = uint8_t(v >> 8);
}

static void Put32(uint8_t* p, uint32_t v) {
    for (int i = 0; i < 4; ++i) {
        p[i] = uint8_t(v >> (8 * i));
    }
}

/// 2-bit colour indices, texel t in bits 2t..2t+1.
static uint32_t ColorIndices(const int idx[16]) {
    uint32_t bits = 0;
    for (int t = 0; t < 16; ++t) {
        bits |= uint32_t(idx[t]) << (2 * t);
    }
    return bits;
}

/// 3-bit channel indices into bytes 2..7 of a BC4-style block.
static void PutChannelIndices(uint8_t* block, const int idx[16]) {
    uint64_t bits = 0;
    for (int t = 0; t < 16; ++t) {
        bits |= uint64_t(idx[t]) << (3 * t);
    }
    for (int i = 0; i < 6; ++i) {
        block[2 + i] = uint8_t(bits >> (8 * i));
    }
}

static void ExpectRgba(const uint8_t* texel, int r, int g, int b, int a) {
    if (texel[0] != r || texel[1] != g || texel[2] != b || texel[3] != a) {
        std::fprintf(stderr, "got %d %d %d %d, want %d %d %d %d\n", texel[0], texel[1], texel[2],
                     texel[3], r, g, b, a);
        assert(false);
    }
}

static void TestBc1() {
    const int idx[16] = {0, 1, 2, 3, 3, 2, 1, 0, 0, 0, 0, 0, 1, 1, 1, 1};
    uint8_t block[8];
    uint8_t out[16 * 4];

    // Four-colour mode (c0 > c1): red and blue, thirds between them.
    Put16(block, Rgb565(31, 0, 0));
    Put16(block + 2, Rgb565(0, 0, 31));
    Put32(block + 4, ColorIndices(idx));
    DecodeBlock(Kind::Bc1, block, out);
    ExpectRgba(out + 0 * 4, 255, 0, 0, 255);
    ExpectRgba(out + 1 * 4, 0, 0, 255, 255);
    ExpectRgba(out + 2 * 4, 170, 0, 85, 255);
    ExpectRgba(out + 3 * 4, 85, 0, 170, 255);
    ExpectRgba(out + 7 * 4, 255, 0, 0, 255);
    ExpectRgba(out + 15 * 4, 0, 0, 255, 255);

    // Three-colour mode (c0 <= c1): the midpoint, and transparent black for index 3.
    Put16(block, Rgb565(0, 0, 31));
    Put16(block + 2, Rgb565(31, 63, 0));
    DecodeBlock(Kind::Bc1, block, out);
    ExpectRgba(out + 0 * 4, 0, 0, 255, 255);
    ExpectRgba(out + 1 * 4, 255, 255, 0, 255);
    ExpectRgba(out + 2 * 4, 128, 128, 128, 255);
    ExpectRgba(out + 3 * 4, 0, 0, 0, 0);

    // 565 expansion replicates the top bits: 0b10000 -> 0b10000100 = 132.
    Put16(block, Rgb565(16, 32, 16));
    Put16(block + 2, Rgb565(16, 32, 16));
    Put32(block + 4, 0);
    DecodeBlock(Kind::Bc1, block, out);
    ExpectRgba(out, 132, 130, 132, 255);
}

static void TestBc2AndBc3() {
    uint8_t block[16];
    uint8_t out[16 * 4];
    // BC2: explicit alpha nibbles 0..15 by texel; colour block equal endpoints (white).
    for (int t = 0; t < 16; t += 2) {
        block[t / 2] = uint8_t(t | ((t + 1) << 4));
    }
    Put16(block + 8, 0xffff);
    Put16(block + 10, 0xffff);
    Put32(block + 12, 0);
    DecodeBlock(Kind::Bc2, block, out);
    for (int t = 0; t < 16; ++t) {
        ExpectRgba(out + t * 4, 255, 255, 255, t * 17);
    }

    // BC2/BC3 colour is always four-colour, even with c0 <= c1 (index 3 is not transparent).
    Put16(block + 8, Rgb565(0, 0, 31));
    Put16(block + 10, Rgb565(31, 0, 0));
    Put32(block + 12, 0xffffffffu);
    DecodeBlock(Kind::Bc2, block, out);
    ExpectRgba(out + 0, 170, 0, 85, 1 * 0);

    // BC3: interpolated alpha, eight-value mode (a0 > a1).
    block[0] = 255;
    block[1] = 0;
    const int aidx[16] = {0, 1, 2, 3, 4, 5, 6, 7, 0, 0, 0, 0, 0, 0, 0, 1};
    PutChannelIndices(block, aidx);
    Put16(block + 8, Rgb565(0, 63, 0));
    Put16(block + 10, Rgb565(0, 0, 0));
    Put32(block + 12, 0);
    DecodeBlock(Kind::Bc3, block, out);
    const int want_alpha[8] = {255, 0, 219, 182, 146, 109, 73, 36};
    for (int t = 0; t < 8; ++t) {
        ExpectRgba(out + t * 4, 0, 255, 0, want_alpha[t]);
    }
    ExpectRgba(out + 15 * 4, 0, 255, 0, 0);
}

static void TestBc4AndBc5() {
    uint8_t block[16];
    uint8_t out[16 * 2];
    // Six-value mode (e0 <= e1): four steps, then 0 and 255.
    block[0] = 10;
    block[1] = 60;
    const int idx[16] = {0, 1, 2, 3, 4, 5, 6, 7, 7, 6, 5, 4, 3, 2, 1, 0};
    PutChannelIndices(block, idx);
    DecodeBlock(Kind::Bc4Unorm, block, out);
    const int want[8] = {10, 60, 20, 30, 40, 50, 0, 255};
    for (int t = 0; t < 8; ++t) {
        assert(out[t] == want[t]);
        assert(out[15 - t] == want[t]);
    }

    // Signed: -128 reads as -127; eight-value mode with 127 > -127.
    block[0] = 127;
    block[1] = 0x80;
    const int sidx[16] = {0, 1, 2, 3, 4, 5, 6, 7, 0, 0, 0, 0, 0, 0, 0, 0};
    PutChannelIndices(block, sidx);
    DecodeBlock(Kind::Bc4Snorm, block, out);
    const int want_s[8] = {127, -127, 91, 54, 18, -18, -54, -91};
    for (int t = 0; t < 8; ++t) {
        assert(int8_t(out[t]) == want_s[t]);
    }

    // BC5: two BC4 blocks, interleaved as RG.
    block[0] = 200;
    block[1] = 200;
    const int zero[16] = {};
    PutChannelIndices(block, zero);
    block[8] = 7;
    block[9] = 7;
    PutChannelIndices(block + 8, zero);
    DecodeBlock(Kind::Bc5Unorm, block, out);
    for (int t = 0; t < 16; ++t) {
        assert(out[2 * t] == 200 && out[2 * t + 1] == 7);
    }
}

static void TestMicroTiling() {
    // Thin micro tiling interleaves x0 y0 x1 y1 x2 y2.
    assert(MicroTileElementIndex(1, 0, 0, MicroThin, 1, 64) == 1);
    assert(MicroTileElementIndex(0, 1, 0, MicroThin, 1, 64) == 2);
    assert(MicroTileElementIndex(2, 0, 0, MicroThin, 1, 64) == 4);
    assert(MicroTileElementIndex(7, 7, 0, MicroThin, 1, 64) == 63);
    // Display micro tiling for 64-bit elements: x0 y0 x1 x2 y1 y2.
    assert(MicroTileElementIndex(4, 0, 0, MicroDisplay, 1, 64) == 8);
    assert(MicroTileElementIndex(0, 2, 0, MicroDisplay, 1, 64) == 16);
    // ... and for 128-bit elements: y0 x0 x1 x2 y1 y2.
    assert(MicroTileElementIndex(1, 0, 0, MicroDisplay, 1, 128) == 2);
    assert(MicroTileElementIndex(0, 1, 0, MicroDisplay, 1, 128) == 1);

    // Every element of a 16x8 two-tile surface maps to a distinct 8-byte slot inside it.
    std::set<uint64_t> offsets;
    for (uint32_t y = 0; y < 8; ++y) {
        for (uint32_t x = 0; x < 16; ++x) {
            const uint64_t offset = MicroTiledOffset(x, y, 0, 16, 8, MicroThin, 1, 64);
            assert(offset % 8 == 0 && offset < 16 * 8 * 8);
            offsets.insert(offset);
        }
    }
    assert(offsets.size() == 16 * 8);
    // Second tile starts after the first 64 elements; the next slice after pitch*height.
    assert(MicroTiledOffset(8, 0, 0, 16, 8, MicroThin, 1, 64) == 64 * 8);
    assert(MicroTiledOffset(0, 0, 1, 16, 8, MicroThin, 1, 64) == 16 * 8 * 8);
    // A 512x1 BC1 1D texture (128 blocks, pitch 128, height 8 blocks): block x of row 0.
    assert(MicroTiledOffset(9, 0, 0, 128, 8, MicroThin, 1, 64) == (64 + 1) * 8);
    assert(MicroTiledOffset(0, 0, 3, 128, 8, MicroThin, 1, 64) == 3ull * 128 * 8 * 8);
}

int main() {
    TestBc1();
    TestBc2AndBc3();
    TestBc4AndBc5();
    TestMicroTiling();
    std::puts("bc-decode-test: ok");
    return 0;
}
