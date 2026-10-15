"""Find the instructions some hosts lack (MOVBE, LZCNT, BMI1, F16C: Haswell; SSE4A: AMD only) in the linked image.

scan_segment decodes one executable segment with iced-x86 and classifies each
site: JMP (5+ bytes, patched with a jump to a stub), JMP_RELOCATE (4 bytes,
the jump also covers the plain next instruction, copied into the stub), INT3
(4 bytes, trapped at run time) or UNSUPPORTED (an encoding the stubs do not
emulate: segment override, LOCK, 32-bit addressing, F16C, MOVNTSS or MOVNTSD).
SSE4A EXTRQ/INSERTQ keep their XMM numbers in dst/src1 and, in the immediate
forms, length | index << 8 in disp.

Usage: isa_rewrite.py out/boot-linked.bin --out out [--red-zone]
Reads the G3PROBE5 image, scans its executable segments and writes
<out>/isa-rewrite.bin (format G3ISA1: HEADER, then one RECORD per site). --red-zone appends the
Windows red-zone protection section (scripts/red_zone.py, G3RZONE1; its size is the last header
field). A table that already matches the image and the options is kept (the analysis takes seconds).
"""
import argparse
from dataclasses import dataclass
import hashlib
from pathlib import Path
import struct
import sys
from prepare import span, unpack
import red_zone

try:
    import iced_x86
    from iced_x86 import Code, Decoder, FlowControl, Mnemonic, OpKind, Register, RegisterExt
except ImportError:
    iced_x86 = None

(KIND_MOVBE_LOAD, KIND_MOVBE_STORE, KIND_LZCNT, KIND_TZCNT, KIND_ANDN, KIND_BEXTR,
 KIND_BLSI, KIND_BLSMSK, KIND_BLSR, KIND_VCVTPH2PS, KIND_VCVTPS2PH,
 KIND_EXTRQ, KIND_EXTRQ_IMM, KIND_INSERTQ, KIND_INSERTQ_IMM, KIND_MOVNTSS, KIND_MOVNTSD) = range(1, 18)
MODE_JMP, MODE_JMP_RELOCATE, MODE_INT3, MODE_UNSUPPORTED = range(4)
MEM_RELOCATED_FAULTABLE = 2
NO_REG = 0xFF
HEADER = struct.Struct('<8sQ32sIIQ')                # magic, image size, sha256, sites, unsupported, red-zone bytes
RECORD = struct.Struct('<QBBBBBBBBBBBB4xq15sB15sx')  # mirrors IsaSite in the C loader
SET_OF_KIND = {KIND_MOVBE_LOAD: 'MOVBE', KIND_MOVBE_STORE: 'MOVBE', KIND_LZCNT: 'LZCNT',
               KIND_TZCNT: 'BMI1', KIND_ANDN: 'BMI1', KIND_BEXTR: 'BMI1', KIND_BLSI: 'BMI1',
               KIND_BLSMSK: 'BMI1', KIND_BLSR: 'BMI1', KIND_VCVTPH2PS: 'F16C', KIND_VCVTPS2PH: 'F16C',
               KIND_EXTRQ: 'SSE4A', KIND_EXTRQ_IMM: 'SSE4A', KIND_INSERTQ: 'SSE4A', KIND_INSERTQ_IMM: 'SSE4A',
               KIND_MOVNTSS: 'SSE4A', KIND_MOVNTSD: 'SSE4A'}


@dataclass
class Site:
    offset: int
    length: int
    kind: int
    width: int
    mode: int
    dst: int
    src1: int
    src2: int
    mem_slot: int   # 0 none, 1 dst, 2 src
    base: int
    index: int
    scale: int
    mem_flags: int  # bit 0: RIP-relative, disp is the absolute target; bit 1: the relocated
                    # instruction can fault on a tracked page (red-zone protection steps rsp over it)
    disp: int
    original: bytes
    relocated: bytes


def kind_of(ins):
    m = ins.mnemonic
    if m == Mnemonic.MOVBE:
        return KIND_MOVBE_LOAD if ins.op0_kind == OpKind.REGISTER else KIND_MOVBE_STORE
    if m == Mnemonic.EXTRQ:
        return KIND_EXTRQ_IMM if ins.code == Code.EXTRQ_XMM_IMM8_IMM8 else KIND_EXTRQ
    if m == Mnemonic.INSERTQ:
        return KIND_INSERTQ_IMM if ins.code == Code.INSERTQ_XMM_XMM_IMM8_IMM8 else KIND_INSERTQ
    return {Mnemonic.LZCNT: KIND_LZCNT, Mnemonic.TZCNT: KIND_TZCNT, Mnemonic.ANDN: KIND_ANDN,
            Mnemonic.BEXTR: KIND_BEXTR, Mnemonic.BLSI: KIND_BLSI, Mnemonic.BLSMSK: KIND_BLSMSK,
            Mnemonic.BLSR: KIND_BLSR, Mnemonic.VCVTPH2PS: KIND_VCVTPH2PS,
            Mnemonic.VCVTPS2PH: KIND_VCVTPS2PH, Mnemonic.MOVNTSS: KIND_MOVNTSS,
            Mnemonic.MOVNTSD: KIND_MOVNTSD}.get(m)


def register_number(reg):
    return NO_REG if reg == Register.NONE else RegisterExt.number(reg)


def site_of(ins, kind, original):
    """Operands in order fill dst, src1, src2; the memory operand leaves its slot NO_REG."""
    regs, mem_slot, width = [NO_REG] * 3, 0, 0
    for i in range(ins.op_count):
        if ins.op_kind(i) == OpKind.REGISTER:
            regs[i] = register_number(ins.op_register(i))
            width = width or RegisterExt.size(ins.op_register(i)) * 8
        elif ins.op_kind(i) == OpKind.MEMORY:
            mem_slot = 1 if i == 0 else 2
    if SET_OF_KIND[kind] in ('F16C', 'SSE4A'):
        width = 0
    base = index = NO_REG
    scale = mem_flags = disp = 0
    unsupported = (SET_OF_KIND[kind] == 'F16C' or kind in (KIND_MOVNTSS, KIND_MOVNTSD)
                   or ins.segment_prefix != Register.NONE or ins.has_lock_prefix)
    if kind in (KIND_EXTRQ_IMM, KIND_INSERTQ_IMM):
        disp = ins.immediate8 | ins.immediate8_2nd << 8   # field length, bit index
    if mem_slot:
        unsupported |= any(r != Register.NONE and RegisterExt.size(r) != 8 for r in (ins.memory_base, ins.memory_index))
        scale = ins.memory_index_scale
        if ins.is_ip_rel_memory_operand:
            mem_flags, disp = 1, ins.ip_rel_memory_address
        else:
            base, index = register_number(ins.memory_base), register_number(ins.memory_index)
            disp = ins.memory_displacement  # iced reports it as u64; the table stores int64
            disp -= (1 << 64) if disp >= 1 << 63 else 0
    mode = MODE_UNSUPPORTED if unsupported else MODE_JMP if ins.len >= 5 else MODE_INT3
    return Site(ins.ip, ins.len, kind, width, mode, *regs, mem_slot, base, index, scale, mem_flags, disp, original, b'')


def is_plain(ins, end):
    """Candidate to be moved into a 4-byte site's stub (branch targets are checked later)."""
    return (ins.code != Code.INVALID and ins.flow_control == FlowControl.NEXT and not ins.is_ip_rel_memory_operand
            and not ins.has_rep_prefix and not ins.has_repne_prefix and kind_of(ins) is None and ins.next_ip <= end)


def scan_segment(code, address):
    """Sites of one executable segment loaded at address, in address order."""
    if iced_x86 is None:
        raise RuntimeError('iced_x86 is not installed')
    end = address + len(code)
    sites, targets, candidates = [], set(), []
    pending = None  # 4-byte site waiting for the instruction after it
    for ins in Decoder(64, code, ip=address):
        if ins.flow_control in (FlowControl.UNCONDITIONAL_BRANCH, FlowControl.CONDITIONAL_BRANCH, FlowControl.CALL) \
                and ins.op0_kind == OpKind.NEAR_BRANCH64:
            targets.add(ins.near_branch_target)
        if pending is not None and is_plain(ins, end):
            candidates.append((pending, ins.ip, code[ins.ip - address:ins.next_ip - address]))
        pending = None
        kind = kind_of(ins)
        if kind is None:
            continue
        site = site_of(ins, kind, code[ins.ip - address:ins.next_ip - address])
        sites.append(site)
        if site.mode == MODE_INT3 and ins.len == 4:
            pending = site
    factory = iced_x86.InstructionInfoFactory()
    for site, ip, relocated in candidates:
        if ip not in targets:
            site.mode, site.relocated = MODE_JMP_RELOCATE, relocated
            follower = red_zone.describe(Decoder(64, relocated, ip=ip).decode(), factory)
            if follower.faultable and not follower.uses_sp:
                site.mem_flags |= MEM_RELOCATED_FAULTABLE
    return sites


def read_boot_linked(data):
    """Segments (address, size, flags) and image of a G3PROBE5 file, in link_modules.py's order."""
    magic, size, entry, ns, nr, ni, flags = unpack('<8s6Q', data, 0)
    if magic != b'G3PROBE5':
        raise ValueError('not a G3PROBE5 image')
    pos = 56 + 8 + 32   # header, procparam, main TLS
    nmodules, = unpack('<Q', data, pos)
    pos += 8 + nmodules * 56
    nbindings, = unpack('<Q', data, pos)
    pos += 8 + nbindings * 24
    segments = [unpack('<3Q', data, pos + i * 24) for i in range(ns)]
    pos += ns * 24 + ni * 128 + nr * 32
    return segments, span(data, pos, size)


def encode_table(image, sites, red_zone_section=b'', digest=None):
    unsupported = sum(s.mode == MODE_UNSUPPORTED for s in sites)
    digest = digest or hashlib.sha256(image).digest()
    table = [HEADER.pack(b'G3ISA1\0\0', len(image), digest, len(sites), unsupported, len(red_zone_section))]
    for s in sorted(sites, key=lambda s: s.offset):   # the C loader rejects records out of order
        table.append(RECORD.pack(s.offset, s.length, s.kind, s.width, s.mode, s.dst, s.src1, s.src2, s.mem_slot,
                                 s.base, s.index, s.scale, s.mem_flags, s.disp, s.original, len(s.relocated),
                                 s.relocated))
    table.append(red_zone_section)
    return b''.join(table)


def summary(sites):
    sets = [SET_OF_KIND[s.kind] for s in sites]
    modes = [s.mode for s in sites]
    per_set = ', '.join(f'{name} {sets.count(name)}' for name in ('MOVBE', 'BMI1', 'LZCNT', 'F16C', 'SSE4A'))
    return (f'ISA: {len(sites)} sites ({per_set}); {modes.count(MODE_JMP_RELOCATE)} relocated, '
            f'{modes.count(MODE_INT3)} int3, {modes.count(MODE_UNSUPPORTED)} unsupported')


def fixture_code():
    """Code behind tests/data/isa_fixture.bin, loaded by the C loader test."""
    return b''.join(bytes.fromhex(h) for h in (
        'c4e270f3c9',              # blsr ecx, ecx                 JMP
        '0f38f108', '90',          # movbe [rax], ecx; nop         JMP_RELOCATE
        'c4e270f7050c000000',      # bextr eax, [rip+0xc], ecx     JMP
        '66f30fbdc1',              # lzcnt ax, cx                  JMP
        'f3480fbcc1',              # tzcnt rax, rcx                JMP
        '0f38f108', 'ebfe',        # movbe [rax], ecx; jmp $       INT3
        'c3'))                     # ret


def isa_bytes(sites):
    """Image bytes the emulation rewrite may patch (sites and the instructions they relocate)."""
    covered = set()
    for s in sites:
        if s.mode == MODE_UNSUPPORTED:
            continue
        covered.update(range(s.offset, s.offset + max(s.length, 4 + len(s.relocated))))
    return frozenset(covered)


def cached(path, digest, image_size, red_zone_wanted):
    """The summary of an existing table for this image and options, or None."""
    try:
        data = path.read_bytes()
        magic, size, sha, count, _, rz_bytes = HEADER.unpack_from(data, 0)
        if magic != b'G3ISA1\0\0' or size != image_size or sha != digest or bool(rz_bytes) != red_zone_wanted:
            return None
        if len(data) != HEADER.size + RECORD.size * count + rz_bytes:
            return None
        if not rz_bytes:
            return None                  # unversioned: always rebuilt (3 s)
        section = data[HEADER.size + RECORD.size * count:]
        magic, _, _, _, _, version, crc = red_zone.SECTION.unpack_from(section, 0)
        if magic != red_zone.MAGIC or version != red_zone.VERSION or crc != red_zone.source_crc():
            return None
        stats = dict(zip(red_zone.STATS_FIELDS, red_zone.STATS.unpack_from(section, red_zone.SECTION.size)))
        return f'ISA: {count} sites (table kept, it matches the image)\n{red_zone.summary(stats)}'
    except (OSError, struct.error, ValueError):
        return None


def main(argv):
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('boot', type=Path)
    p.add_argument('--out', type=Path, default=Path(__file__).resolve().parent.parent / 'out')
    p.add_argument('--red-zone', action='store_true', help='append the Windows red-zone protection section')
    a = p.parse_args(argv)
    try:
        if iced_x86 is None:
            raise RuntimeError('iced_x86 is not installed')
        segments, image = read_boot_linked(a.boot.read_bytes())
        digest = hashlib.sha256(image).digest()
        table = a.out / 'isa-rewrite.bin'
        kept = cached(table, digest, len(image), a.red_zone)
        if kept:
            print(kept)
            return 0
        sites = [site for address, size, flags in segments if flags & 1
                 for site in scan_segment(span(image, address, size), address)]
        section, rz = b'', None
        if a.red_zone:
            try:
                builder, rz = red_zone.analyze(image, segments, red_zone.code_offset_for(len(image), len(sites)),
                                               isa_bytes(sites))
                section = red_zone.encode_section(builder, rz)
            except (ValueError, RuntimeError) as e:
                print(f'Red zone: protection unavailable ({e})')
        table.write_bytes(encode_table(image, sites, section, digest))
    except (OSError, ValueError, RuntimeError) as e:
        print(f'ISA: rewrite table unavailable ({e})')
        return 1
    print(summary(sites))
    if rz:
        print(red_zone.summary(rz))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
