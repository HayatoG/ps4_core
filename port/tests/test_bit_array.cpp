// SPDX-License-Identifier: GPL-2.0-or-later
// BitArray::AnyInRange (common/bit_array.h): the AVX2 middle loop must agree with a plain scan
// for every start/end and for words set anywhere in the range or just outside it.
#include <cassert>
#include <cstdio>
#include <random>
#include "common/bit_array.h"

template <size_t N>
static bool Reference(const Common::BitArray<N>& bits, size_t start, size_t end) {
    for (size_t i = start; i < end; ++i) {
        if (bits.Get(i)) {
            return true;
        }
    }
    return false;
}

int main() {
    using Bits = Common::BitArray<1024>; // one 4 MB region of 4 KiB pages, as RegionBits
    std::mt19937 rng{42};
    for (int round = 0; round < 2000; ++round) {
        Bits bits;
        const int set = static_cast<int>(rng() % 4);
        for (int i = 0; i < set; ++i) {
            bits.Set(rng() % 1024);
        }
        for (int q = 0; q < 16; ++q) {
            const size_t a = rng() % 1024;
            const size_t b = a + 1 + rng() % (1024 - a);
            assert(bits.AnyInRange(a, b) == Reference(bits, a, b));
        }
        assert(bits.Any() == Reference(bits, 0, 1024));
    }
    // Ranges that cross four or more whole words exercise the vector loop exactly.
    for (size_t bit = 0; bit < 1024; bit += 7) {
        Bits bits;
        bits.Set(bit);
        for (size_t a = 0; a < 1024; a += 64) {
            for (size_t b = a + 1; b <= 1024; b += 61) {
                assert(bits.AnyInRange(a, b) == (a <= bit && bit < b));
            }
        }
    }
    std::puts("bit-array-test: ok");
    return 0;
}
