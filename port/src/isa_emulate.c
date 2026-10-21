#include "isa_emulate.h"

static unsigned char *isa_image;

void isa_emulate_set_image(unsigned char *image) { isa_image = image; }

static uint64_t width_mask(const IsaSite *site) {
    return site->width == 64 ? ~0ull : (1ull << site->width) - 1;
}

static unsigned char *effective_address(const IsaSite *site, const IsaRegs *regs) {
    if (site->mem_flags & ISA_MEM_RIP_RELATIVE) return isa_image + site->disp;
    uint64_t address = (uint64_t)site->disp;
    if (site->base != ISA_NO_REG) address += regs->gpr[site->base];
    if (site->index != ISA_NO_REG) address += regs->gpr[site->index] * site->scale;
    return (unsigned char *)(uintptr_t)address;
}

/* Unaligned accesses are valid on x86; the constant-size copies inline to plain moves. */
static uint64_t load(const IsaSite *site, const IsaRegs *regs) {
    unsigned char *at = effective_address(site, regs);
    if (site->width == 16) { uint16_t v; __builtin_memcpy(&v, at, 2); return v; }
    if (site->width == 32) { uint32_t v; __builtin_memcpy(&v, at, 4); return v; }
    uint64_t v;
    __builtin_memcpy(&v, at, 8);
    return v;
}

static void store(const IsaSite *site, const IsaRegs *regs, uint64_t value) {
    unsigned char *at = effective_address(site, regs);
    if (site->width == 16) { uint16_t v = (uint16_t)value; __builtin_memcpy(at, &v, 2); }
    else if (site->width == 32) { uint32_t v = (uint32_t)value; __builtin_memcpy(at, &v, 4); }
    else __builtin_memcpy(at, &value, 8);
}

/* The operand in a register slot; a slot left NO_REG holds the memory operand. */
static uint64_t read_operand(const IsaSite *site, const IsaRegs *regs, uint8_t reg) {
    if (reg == ISA_NO_REG) return load(site, regs);
    return regs->gpr[reg] & width_mask(site);
}

/* 64-bit writes replace the register, 32-bit ones zero-extend, 16-bit ones keep bits 16-63. */
static void write_dst(const IsaSite *site, IsaRegs *regs, uint64_t value) {
    if (site->dst == ISA_NO_REG) { store(site, regs, value); return; }
    uint64_t *reg = &regs->gpr[site->dst];
    if (site->width == 64) *reg = value;
    else if (site->width == 32) *reg = (uint32_t)value;
    else *reg = (*reg & ~0xffffull) | (value & 0xffff);
}

static void set_flags(IsaRegs *regs, uint64_t mask, uint64_t value) {
    regs->rflags = (regs->rflags & ~mask) | (value & mask);
}

static uint64_t byte_swap(const IsaSite *site, uint64_t value) {
    if (site->width == 16) return __builtin_bswap16((uint16_t)value);
    if (site->width == 32) return __builtin_bswap32((uint32_t)value);
    return __builtin_bswap64(value);
}

static uint64_t zf_sf(const IsaSite *site, uint64_t result) {
    return (result == 0 ? ISA_ZF : 0) | (result >> (site->width - 1) & 1 ? ISA_SF : 0);
}

uint64_t isa_defined_flags(uint8_t kind) {
    switch (kind) {
    case ISA_ANDN: case ISA_BLSI: case ISA_BLSMSK: case ISA_BLSR: return ISA_SF | ISA_ZF | ISA_CF | ISA_OF;
    case ISA_BEXTR: return ISA_ZF | ISA_CF | ISA_OF;
    case ISA_LZCNT: case ISA_TZCNT: return ISA_CF | ISA_ZF;
    default: return 0;
    }
}

void ABI isa_emulate(const IsaSite *site, IsaRegs *regs) {
    uint64_t mask = width_mask(site), defined = isa_defined_flags(site->kind);
    uint64_t width = site->width, source, result, flags;
    switch (site->kind) {
    case ISA_MOVBE_LOAD:
    case ISA_MOVBE_STORE:
        write_dst(site, regs, byte_swap(site, read_operand(site, regs, site->src1)));
        return;
    case ISA_LZCNT:
    case ISA_TZCNT:
        source = read_operand(site, regs, site->src1);
        if (source == 0) result = width;
        else if (site->kind == ISA_LZCNT) result = (uint64_t)__builtin_clzll(source) - (64 - width);
        else result = (uint64_t)__builtin_ctzll(source);
        flags = (source == 0 ? ISA_CF : 0) | (result == 0 ? ISA_ZF : 0);
        break;
    case ISA_ANDN:
        result = ~read_operand(site, regs, site->src1) & read_operand(site, regs, site->src2) & mask;
        flags = zf_sf(site, result);
        break;
    case ISA_BEXTR: {
        source = read_operand(site, regs, site->src1);
        uint64_t control = read_operand(site, regs, site->src2);
        uint64_t start = control & 0xff, length = control >> 8 & 0xff;
        result = 0;
        if (start < width && length != 0) {
            if (length > width - start) length = width - start;
            result = source >> start & (length == 64 ? ~0ull : (1ull << length) - 1);
        }
        flags = result == 0 ? ISA_ZF : 0;
        break;
    }
    case ISA_BLSI:
        source = read_operand(site, regs, site->src1);
        result = source & (0 - source) & mask;
        flags = zf_sf(site, result) | (source != 0 ? ISA_CF : 0);
        break;
    case ISA_BLSMSK:
        source = read_operand(site, regs, site->src1);
        result = (source ^ (source - 1)) & mask;
        flags = (zf_sf(site, result) & ISA_SF) | (source == 0 ? ISA_CF : 0);
        break;
    case ISA_BLSR:
        source = read_operand(site, regs, site->src1);
        result = source & (source - 1) & mask;
        flags = zf_sf(site, result) | (source == 0 ? ISA_CF : 0);
        break;
    default:   /* F16C goes through isa_emulate_f16c, SSE4A through isa_emulate_sse4a */
        return;
    }
    write_dst(site, regs, result);
    set_flags(regs, defined, flags);
}

int isa_kind_is_sse4a(uint8_t kind) { return kind >= ISA_EXTRQ && kind <= ISA_MOVNTSD; }

/* Field length (0 means 64) and bit index: 6 bits each, the upper bits of their bytes are ignored. */
static uint64_t field_mask(unsigned length) { return length == 0 ? ~0ull : (1ull << length) - 1; }

void ABI isa_emulate_sse4a(const IsaSite *site, uint64_t xmm[4]) {
    uint64_t control;
    switch (site->kind) {
    case ISA_EXTRQ: control = xmm[2]; break;                   /* xmm2[5:0], xmm2[13:8] */
    case ISA_INSERTQ: control = xmm[3]; break;                 /* xmm2[69:64], xmm2[77:72] */
    case ISA_EXTRQ_IMM: case ISA_INSERTQ_IMM: control = (uint64_t)site->disp; break;
    default: return;                                           /* MOVNTSS/MOVNTSD are never applied */
    }
    const unsigned length = control & 63, index = control >> 8 & 63;
    if (site->kind == ISA_EXTRQ || site->kind == ISA_EXTRQ_IMM) {
        xmm[0] = xmm[0] >> index & field_mask(length);
    } else {
        const uint64_t mask = field_mask(length) << index;     /* bits past 63 are dropped */
        xmm[0] = (xmm[0] & ~mask) | (xmm[2] << index & mask);
    }
    xmm[1] = 0;
}

/* ---- F16C (VCVTPH2PS, VCVTPS2PH) in software: no XMM register is used ---- */

int isa_kind_is_f16c(uint8_t kind) { return kind == ISA_VCVTPH2PS || kind == ISA_VCVTPS2PH; }

/* stmxcsr only stores the control word: no XMM/YMM register is read or written. */
static uint32_t read_mxcsr(void) {
    uint32_t value;
    __asm__ volatile("stmxcsr %0" : "=m"(value));
    return value;
}

uint32_t isa_half_to_float(uint16_t half) {
    const uint32_t sign = (uint32_t)(half & 0x8000) << 16, exponent = half >> 10 & 31, mantissa = half & 0x3ff;
    if (exponent == 31)   /* Inf; a NaN keeps its payload and is quieted */
        return sign | 0x7f800000u | (mantissa ? 0x400000u | mantissa << 13 : 0);
    if (exponent != 0) return sign | (exponent + 112) << 23 | mantissa << 13;
    if (mantissa == 0) return sign;
    /* Denormal half: always a normal float (MXCSR.DAZ does not apply to this instruction). */
    int shift = __builtin_clz(mantissa) - 21;            /* moves the leading 1 to bit 10 */
    return sign | (uint32_t)(113 - shift) << 23 | (mantissa << shift & 0x3ff) << 13;
}

uint16_t isa_float_to_half(uint32_t single, unsigned rounding, int daz) {
    const uint16_t sign = (uint16_t)(single >> 16 & 0x8000);
    const uint32_t exponent = single >> 23 & 0xff, mantissa = single & 0x7fffff;
    if (exponent == 255)  /* Inf; a NaN keeps the top 10 payload bits and is quieted */
        return (uint16_t)(sign | 0x7c00 | (mantissa ? 0x200 | mantissa >> 13 : 0));
    if (exponent == 0 && (mantissa == 0 || daz)) return sign;   /* DAZ: a denormal input is zero */
    /* value = full * 2^e; the half's unit is 2^unit: 2^-24 for denormals, 2^(E - 10) for normals. */
    const uint32_t full = exponent ? mantissa | 0x800000 : mantissa;
    const int e = (exponent ? (int)exponent : 1) - 150;
    const int top = e + 31 - __builtin_clz(full);         /* floor(log2 value) */
    const int unit = top - 10 > -24 ? top - 10 : -24;
    const int shift = unit - e;                           /* >= 13: every float has 24 bits */
    uint32_t q = 0, round = 0, sticky = 1;                /* past 24 bits: below half a unit, inexact */
    if (shift <= 24) {
        q = full >> shift;
        round = full >> (shift - 1) & 1;
        sticky = (full & ((1u << (shift - 1)) - 1)) != 0;
    }
    switch (rounding & 3) {
    case 0: q += round & (sticky | (q & 1)); break;       /* nearest, ties to even */
    case 1: q += sign && (round | sticky); break;         /* toward -Inf */
    case 2: q += !sign && (round | sticky); break;        /* toward +Inf */
    default: break;                                       /* toward zero */
    }
    const uint32_t bits = (uint32_t)(unit + 24) * 0x400 + q;   /* a carry out of the mantissa bumps the exponent */
    if (bits >= 0x7c00) {                                 /* overflow: Inf or the largest finite half */
        const int to_inf = (rounding & 3) == 0 || ((rounding & 3) == 1 && sign) || ((rounding & 3) == 2 && !sign);
        return (uint16_t)(sign | (to_inf ? 0x7c00 : 0x7bff));
    }
    return (uint16_t)(sign | bits);
}

/* Fixed-size copies only: a variable length could become a libc memcpy call, which may use XMM. */
static void copy_halves(void *to, const void *from, int wide) {
    if (wide) __builtin_memcpy(to, from, 16);
    else __builtin_memcpy(to, from, 8);
}

void ABI isa_emulate_f16c(const IsaSite *site, const IsaRegs *regs, uint8_t ymm[64]) {
    const int wide = site->width == 32;   /* VEX.256: 8 lanes */
    const unsigned lanes = wide ? 8 : 4;
    uint64_t result[4] = {0, 0, 0, 0};                   /* the destination register: VEX zeroes the bits above */
    uint16_t half[8] = {0, 0, 0, 0, 0, 0, 0, 0};
    if (site->kind == ISA_VCVTPH2PS) {
        copy_halves(half, site->src1 == ISA_NO_REG ? effective_address(site, regs) : ymm + 32, wide);
        for (unsigned i = 0; i < lanes; i++) {
            const uint32_t single = isa_half_to_float(half[i]);
            result[i / 2] |= (uint64_t)single << 32 * (i & 1);
        }
    } else if (site->kind == ISA_VCVTPS2PH) {
        const uint32_t mxcsr = read_mxcsr();
        /* imm8[2] = 1: MXCSR.RC, else imm8[1:0]; imm8[3] only masks the precision exception. MXCSR.DAZ
         * applies (denormal inputs become zero), MXCSR.FTZ does not (tiny results stay denormal). */
        const unsigned rounding = site->imm & 4 ? mxcsr >> 13 & 3 : site->imm & 3;
        const int daz = mxcsr >> 6 & 1;
        for (unsigned i = 0; i < lanes; i++) {
            uint32_t single;
            __builtin_memcpy(&single, ymm + 32 + 4 * i, 4);
            half[i] = isa_float_to_half(single, rounding, daz);
        }
        if (site->dst == ISA_NO_REG) {
            copy_halves(effective_address(site, regs), half, wide);
            return;
        }
        copy_halves(result, half, wide);
    } else {
        return;
    }
    __builtin_memcpy(ymm, result, 32);
}
