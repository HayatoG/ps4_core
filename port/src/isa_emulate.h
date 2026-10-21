/* Emulation of the guest's MOVBE, BMI1, LZCNT, F16C and SSE4A instructions for hosts that lack them
 * (Sandy and Ivy Bridge, #33, #66, iss_pc #51; SSE4A: every Intel CPU). Each G3ISA1 record (scripts/isa_rewrite.py) describes one instruction
 * site; a stub calls isa_emulate with the guest's registers. Built with -mgeneral-regs-only and
 * no libc: it runs inside guest code and must leave the XMM/YMM registers untouched. */
#ifndef TLG_ISA_EMULATE_H
#define TLG_ISA_EMULATE_H
#include <stdint.h>
#ifndef ABI               /* same as runtime.h, which needs sigjmp_buf (_GNU_SOURCE) */
#define ABI __attribute__((sysv_abi))
#endif

enum { ISA_MOVBE_LOAD = 1, ISA_MOVBE_STORE, ISA_LZCNT, ISA_TZCNT, ISA_ANDN, ISA_BEXTR, ISA_BLSI, ISA_BLSMSK, ISA_BLSR, ISA_VCVTPH2PS, ISA_VCVTPS2PH,
       /* SSE4A (AMD only). dst/src1 are XMM numbers; the _IMM forms keep length | index << 8 in disp.
        * MOVNTSS/MOVNTSD are only recorded (always UNSUPPORTED): no PS4 game seen uses them.
        * F16C (VEX only): dst/src1 are XMM/YMM numbers (NO_REG for the memory operand), width is the
        * vector size in bytes (16, or 32 with VEX.L = 1), imm the VCVTPS2PH rounding control. */
       ISA_EXTRQ, ISA_EXTRQ_IMM, ISA_INSERTQ, ISA_INSERTQ_IMM, ISA_MOVNTSS, ISA_MOVNTSD };
enum { ISA_MODE_JMP = 0, ISA_MODE_JMP_RELOCATE, ISA_MODE_INT3, ISA_MODE_UNSUPPORTED };
enum { ISA_NO_REG = 0xff, ISA_MEM_NONE = 0, ISA_MEM_DST = 1, ISA_MEM_SRC = 2, ISA_MEM_RIP_RELATIVE = 1 };
typedef struct {                       /* one G3ISA1 record (spec 4.4), as stored in the file */
    uint64_t offset;
    uint8_t length, kind, width, mode, dst, src1, src2, mem_slot, base, index, scale, mem_flags;
    uint8_t imm;                       /* VCVTPS2PH imm8 */
    uint8_t reserved[3];
    int64_t disp;
    uint8_t original[15];
    uint8_t relocated_length;
    uint8_t relocated[15];
    uint8_t reserved2;
} IsaSite;
_Static_assert(sizeof(IsaSite) == 64, "G3ISA1 record");
typedef struct { uint64_t rflags; uint64_t gpr[16]; } IsaRegs;   /* gpr: x86 numbering; gpr[4] = guest rsp */
enum { ISA_CF = 1u << 0, ISA_ZF = 1u << 6, ISA_SF = 1u << 7, ISA_OF = 1u << 11 };
/* Memory operands with mem_flags & ISA_MEM_RIP_RELATIVE address image + disp. */
void isa_emulate_set_image(unsigned char *image);
/* Executes the site's instruction on regs (spec 5.6). */
void ABI isa_emulate(const IsaSite *site, IsaRegs *regs);
/* Flags the instruction defines (the test compares only these). */
uint64_t isa_defined_flags(uint8_t kind);
/* 1 for the SSE4A kinds, which work on XMM registers and go through isa_emulate_sse4a. */
int isa_kind_is_sse4a(uint8_t kind);
/* Executes an EXTRQ/INSERTQ site (AMD APM vol. 4) on xmm: xmm[0], xmm[1] = destination low and high
 * qwords, xmm[2], xmm[3] = source register (unused by EXTRQ imm). Writes xmm[0] and xmm[1] only.
 * Cases the APM leaves undefined follow AMD hardware (Zen 3, checked by tests/test_isa_sse4a.c):
 * the destination's high qword is zeroed, and a field with index + length > 64 is cut at bit 63. */
void ABI isa_emulate_sse4a(const IsaSite *site, uint64_t xmm[4]);
/* 1 for VCVTPH2PS and VCVTPS2PH, which go through isa_emulate_f16c. */
int isa_kind_is_f16c(uint8_t kind);
/* Executes a VCVTPH2PS/VCVTPS2PH site in software (Intel SDM vol. 2; results bit-exact with the
 * hardware, checked by tests/test_isa_f16c.c). ymm[0..31] receives the destination register (all 256
 * bits: VEX zeroes the bits above the result; untouched when the destination is memory), ymm[32..63]
 * holds the source register (unused when the source is memory). Memory operands are addressed with
 * regs (gpr[4] = guest rsp). VCVTPS2PH rounds as imm8[1:0], or MXCSR.RC when imm8[2] is set; MXCSR.DAZ
 * turns denormal inputs into zero, MXCSR.FTZ is ignored. VCVTPH2PS is exact (denormal halves become
 * normal floats, DAZ ignored). NaNs keep their sign and payload and are quieted. The MXCSR exception
 * flags are not updated (no guest code reads them after a conversion). */
void ABI isa_emulate_f16c(const IsaSite *site, const IsaRegs *regs, uint8_t ymm[64]);
/* The scalar conversions (rounding: 0 nearest even, 1 down, 2 up, 3 toward zero). */
uint32_t isa_half_to_float(uint16_t half);
uint16_t isa_float_to_half(uint32_t single, unsigned rounding, int daz);

#endif
