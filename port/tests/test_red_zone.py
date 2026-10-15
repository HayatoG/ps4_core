"""Windows red-zone protection (scripts/red_zone.py): instruction facts, liveness, spans and relays,
the .eh_frame_hdr reader, and the fixture the C test (tests/test_red_zone.c) executes.

Regenerate the fixture: python3 tests/test_red_zone.py --write
"""
from paths import ROOT
import struct
import sys
import unittest
import isa_rewrite
import red_zone

if red_zone.iced_x86:
    from iced_x86 import Decoder, InstructionInfoFactory

FIXTURE_IMAGE = ROOT / 'tests/data/red_zone_image.bin'
FIXTURE_TABLE = ROOT / 'tests/data/red_zone_table.bin'
IMAGE_SIZE, DATA_PAGE, INDEX_AT = 0x2000, 0x1000, 0xf00


def h(text):
    return bytes.fromhex(text.replace(' ', ''))


STORE_RSI_M8 = h('48 89 74 24 f8')    # mov [rsp-8], rsi
LOAD_RAX_M8 = h('48 8b 44 24 f8')     # mov rax, [rsp-8]
STORE_EDI_PTR = h('89 37')            # mov [rdi], esi   (2 bytes, faultable)
RET = h('c3')


def fixture_functions():
    """(name, code builder(ip) -> bytes, offset of the faultable instruction or None, padding after)."""
    def rip_store(ip):   # mov dword [rip + data+0x10], 1 after the red-zone store
        return STORE_RSI_M8 + h('c7 05') + struct.pack('<i', DATA_PAGE + 0x10 - (ip + 5 + 10)) + h('01000000') \
            + LOAD_RAX_M8 + RET
    filler = h('48 89 c0') * 47       # mov rax, rax: 141 bytes, hosts for a relay
    return [
        ('jmp', lambda ip: STORE_RSI_M8 + h('c7 07 01 00 00 00') + LOAD_RAX_M8 + RET, 5, 0),
        ('forward', lambda ip: h('48 89 74 24 f0') + STORE_EDI_PTR + h('48 8b 44 24 f0') + RET, 5, 0),
        ('backward', lambda ip: STORE_RSI_M8 + h('85 f6 74 06 89 f1 89 c8 89 0f') + LOAD_RAX_M8 + RET, 13, 0),
        ('rip', rip_store, 5, 0),
        ('flags', lambda ip: STORE_RSI_M8 + h('83 fe 05') + STORE_EDI_PTR + LOAD_RAX_M8 + h('48 83 d0 00') + RET, 8, 0),
        ('vex', lambda ip: STORE_RSI_M8 + h('c5 f9 6e c6 c5 f8 11 07') + LOAD_RAX_M8 + RET, 9, 0),
        ('push_sub', lambda ip: h('53 48 83 ec 10') + STORE_RSI_M8 + STORE_EDI_PTR + LOAD_RAX_M8
         + h('48 83 c4 10 5b') + RET, 10, 0),
        ('dead', lambda ip: STORE_EDI_PTR + STORE_RSI_M8 + LOAD_RAX_M8 + RET, None, 0),
        ('padding_relay', lambda ip: STORE_RSI_M8 + h('83 fe ff 74 04 eb 00') + STORE_EDI_PTR + LOAD_RAX_M8 + RET,
         12, 32),
        ('host_relay', lambda ip: STORE_RSI_M8 + filler + h('83 fe ff 74 04 eb 00') + STORE_EDI_PTR + LOAD_RAX_M8
         + RET, 5 + len(filler) + 7, 0),
    ]


def fixture():
    """Image (code page, data page), function bounds, index (function offsets, faultable offsets)."""
    image = bytearray(b'\xcc' * DATA_PAGE) + bytearray(IMAGE_SIZE - DATA_PAGE)
    functions, entries, ip = [], [], 0x100
    for name, build, fault, pad in fixture_functions():
        code = build(ip)
        image[ip:ip + len(code)] = code
        functions.append((ip, ip + len(code)))
        entries.append((ip, ip + fault if fault is not None else 0))
        ip += len(code) + pad
    assert ip < INDEX_AT
    index = struct.pack('<I', len(entries)) + b''.join(struct.pack('<II', *e) for e in entries)
    image[INDEX_AT:INDEX_AT + len(index)] = index
    return bytes(image), functions, entries


def fixture_table():
    image, functions, _ = fixture()
    segments = [(0, DATA_PAGE, 5), (DATA_PAGE, IMAGE_SIZE - DATA_PAGE, 6)]
    builder, stats = red_zone.analyze(image, segments, red_zone.code_offset_for(len(image), 0), functions=functions)
    table = bytearray(isa_rewrite.encode_table(image, [], red_zone.encode_section(builder, stats)))
    table[64 + 32:64 + 36] = bytes(4)    # the source CRC (cache key only): the fixture stays put
    return bytes(table), builder, stats


def facts(code, ip=0x1000):
    return red_zone.describe(Decoder(64, code, ip=ip).decode(), InstructionInfoFactory())


def offsets(mask):
    return [b - red_zone.RED_ZONE for b in range(red_zone.BITS) if mask >> b & 1]


def protect(code, start=0x1000):
    """Groups and stats for one function made of code."""
    image = bytes(start) + code + b'\xcc' * 64
    builder, stats = red_zone.analyze(image, [(0, len(image), 5)], 0x100000, functions=[(start, start + len(code))])
    return builder, stats


@unittest.skipUnless(red_zone.iced_x86, 'pip install iced-x86')
class FactsTests(unittest.TestCase):
    def test_disp8_store_defines_the_red_zone_bytes(self):
        op = facts(STORE_RSI_M8)
        self.assertTrue(op.rz)
        self.assertEqual(offsets(op.dfn), list(range(-8, 0)))
        self.assertEqual((op.use, op.faultable, op.unmodeled), (0, False, False))

    def test_disp32_load(self):
        op = facts(h('48 8b 84 24 80 ff ff ff'))      # mov rax, [rsp-0x80] (disp32 form)
        self.assertEqual(offsets(op.use), list(range(-128, -120)))

    def test_below_the_red_zone_is_clipped(self):
        op = facts(h('8b 84 24 00 ff ff ff'))          # mov eax, [rsp-0x100]
        self.assertTrue(op.rz)
        self.assertEqual(op.use, 0)

    def test_sib_index_with_rsp_base_is_unmodeled(self):
        self.assertTrue(facts(h('8b 44 8c f8')).unmodeled)   # mov eax, [rsp+rcx*4-8]

    def test_lea_into_the_red_zone_escapes(self):
        op = facts(h('48 8d 44 24 e0'))                 # lea rax, [rsp-0x20]
        self.assertTrue(op.rz and op.unmodeled)
        op = facts(h('48 8d 64 24 e0'))                 # lea rsp, [rsp-0x20]
        self.assertEqual((op.delta, op.rz, op.unmodeled), (-0x20, False, False))

    def test_vex_and_sse_stores(self):
        self.assertEqual(offsets(facts(h('c5 f8 29 44 24 e0')).dfn), list(range(-32, -16)))   # vmovaps [rsp-0x20], xmm0
        self.assertEqual(offsets(facts(h('c5 fc 11 44 24 c0')).dfn), list(range(-64, -32)))   # vmovups [rsp-0x40], ymm0
        self.assertEqual(offsets(facts(h('f3 0f 11 44 24 fc')).dfn), list(range(-4, 0)))      # movss [rsp-4], xmm0
        self.assertEqual(offsets(facts(h('f2 0f 10 44 24 f0')).use), list(range(-16, -8)))    # movsd xmm0, [rsp-0x10]

    def test_read_modify_write_with_flags(self):
        op = facts(h('83 44 24 f8 01'))                 # add dword [rsp-8], 1
        self.assertEqual((offsets(op.use), offsets(op.dfn)), (list(range(-8, -4)), list(range(-8, -4))))

    def test_stack_pointer_deltas(self):
        self.assertEqual(facts(h('50')).delta, -8)                    # push rax
        self.assertEqual(facts(h('5b')).delta, 8)                     # pop rbx
        self.assertEqual(facts(h('48 83 ec 40')).delta, -0x40)        # sub rsp, 0x40
        self.assertEqual(facts(h('48 83 ec 80')).delta, 0x80)         # sub rsp, -0x80
        self.assertEqual(facts(h('48 81 c4 00 01 00 00')).delta, 0x100)   # add rsp, 0x100
        for code in ('48 83 e4 e0', '48 89 ec', 'c9'):                # and rsp,-32; mov rsp,rbp; leave
            op = facts(h(code))
            self.assertEqual((op.delta, op.sp_unknown), (None, True), code)
        op = facts(h('e8 00 00 00 00'))                               # call: rsp unchanged afterwards
        self.assertEqual((op.delta, op.sp_unknown), (None, False))

    def test_faultable_accesses(self):
        for code in ('8b 07', 'c7 07 01 00 00 00', '8b 05 00 00 00 00', 'f3 a4', 'c5 f8 11 07', 'ff 50 08'):
            self.assertTrue(facts(h(code)).faultable, code)        # [rdi], rip-relative, rep movsb, vex, call [rax+8]
        for code in ('8b 44 24 08', '48 89 44 24 f8', '64 48 8b 04 25 00 00 00 00', '0f 18 07', '50', '48 8d 07'):
            self.assertFalse(facts(h(code)).faultable, code)       # stack, fs:, prefetch, push, lea
        op = facts(h('ff 37'))                                    # push qword [rdi]: faultable, uses rsp
        self.assertTrue(op.faultable and op.uses_sp)


@unittest.skipUnless(red_zone.iced_x86, 'pip install iced-x86')
class PlanTests(unittest.TestCase):
    def test_live_store_is_protected_with_rsp_below_the_red_zone(self):
        code = STORE_RSI_M8 + h('c7 07 01 00 00 00') + LOAD_RAX_M8 + RET
        builder, stats = protect(code)
        self.assertEqual((stats['candidates'], stats['protected'], stats['groups']), (1, 1, 1))
        site, stub_at, stub_len, protected, patches = builder.groups[0]
        self.assertEqual((site, protected), (0x1005, 1))
        (offset, original, new), = patches
        self.assertEqual((offset, original, new[0], len(new)), (0x1005, h('c7 07 01 00 00 00'), 0xe9, 6))
        self.assertEqual(new[5:], b'\x90')
        stub = bytes(builder.code[stub_at:stub_at + stub_len])
        self.assertEqual(stub[:5], h('48 8d 64 24 80'))                       # lea rsp, [rsp-0x80]
        self.assertEqual(stub[5:11], h('c7 07 01 00 00 00'))
        self.assertEqual(stub[11:19], h('48 8d a4 24 80 00 00 00'))            # lea rsp, [rsp+0x80]
        self.assertEqual(stub[19], 0xe9)                                       # jmp back
        back = 0x100000 + stub_at + 24 + struct.unpack_from('<i', stub, 20)[0]
        self.assertEqual(back, 0x100b)

    def test_dead_red_zone_is_not_protected(self):
        builder, stats = protect(STORE_EDI_PTR + STORE_RSI_M8 + LOAD_RAX_M8 + RET)
        self.assertEqual((stats['red_zone_functions'], stats['candidates'], stats['groups']), (1, 0, 0))

    def test_short_site_takes_the_next_instruction(self):
        builder, stats = protect(h('48 89 74 24 f0') + STORE_EDI_PTR + h('48 8b 44 24 f0') + RET)
        (offset, original, new), = builder.groups[0][4]
        self.assertEqual((offset, len(original)), (0x1005, 7))

    def test_branch_target_forces_the_previous_instructions(self):
        builder, stats = protect(STORE_RSI_M8 + h('85 f6 74 06 89 f1 89 c8 89 0f') + LOAD_RAX_M8 + RET)
        (offset, original, new), = builder.groups[0][4]
        self.assertEqual((offset, original), (0x1009, h('89 f1 89 c8 89 0f')))

    def test_relays(self):
        image, functions, entries = fixture()
        _, builder, stats = fixture_table()
        relays = [g for g in builder.groups if len(g[4]) == 2]
        self.assertEqual(len(relays), 2)
        padding, host = relays
        site_patch = next(p for p in padding[4] if p[0] == padding[0])
        self.assertEqual(site_patch[2][0], 0xeb)                     # jmp short
        self.assertEqual(stats['short_relays'], 2)
        self.assertEqual(stats['skipped_unrelocatable'], 0)

    def test_rip_relative_instruction_is_retargeted(self):
        _, builder, _ = fixture_table()
        image, functions, entries = fixture()
        group = next(g for g in builder.groups if g[0] == entries[3][1])
        stub = bytes(builder.code[group[1]:group[1] + group[2]])
        self.assertEqual(stub[5:7], h('c7 05'))
        at = red_zone.code_offset_for(len(image), 0) + group[1] + 5
        self.assertEqual(at + 10 + struct.unpack_from('<i', stub, 7)[0], DATA_PAGE + 0x10)

    def test_conservative_function(self):
        # mov [rsp-8], rsi ; mov [rdi], esi ; jmp rax (unresolved) -> everything live, site protected
        builder, stats = protect(STORE_RSI_M8 + h('c7 07 01 00 00 00 ff e0'))
        self.assertEqual((stats['conservative_functions'], stats['protected']), (1, 1))

    def test_value_stored_before_sub_rsp_and_read_above_rsp_stays_live(self):
        # mov [rsp-8], rsi ; mov dword [rdi], 1 ; sub rsp, 8 ; mov rax, [rsp] ; add rsp, 8 ; ret
        builder, stats = protect(STORE_RSI_M8 + h('c7 07 01 00 00 00 48 83 ec 08 48 8b 04 24 48 83 c4 08') + RET)
        self.assertEqual(stats['protected'], 1)

    def test_rsp_dependent_and_isa_sites_are_reported(self):
        builder, stats = protect(STORE_RSI_M8 + h('ff 37 58') + LOAD_RAX_M8 + RET)   # push [rdi] ; pop rax
        self.assertEqual((stats['candidates'], stats['skipped_stack'], stats['groups']), (1, 1, 0))
        code = STORE_RSI_M8 + h('0f 38 f1 37') + LOAD_RAX_M8 + RET                # movbe [rdi], esi
        image = bytes(0x1000) + code + b'\xcc' * 64
        _, stats = red_zone.analyze(image, [(0, len(image), 5)], 0x100000, frozenset(range(0x1005, 0x1009)),
                                    functions=[(0x1000, 0x1000 + len(code))])
        self.assertEqual((stats['skipped_isa'], stats['protected']), (1, 1))   # protected unless emulated

    def test_jump_table_targets_are_successors(self):
        # cmp ecx, 1 ; ja end ; lea rdx, [rip+table] ; movsxd rax, [rdx+rcx*4] ; add rax, rdx ; jmp rax ;
        # case0: mov [rdi], esi ; mov rax, [rsp-8] ; ret ; case1: mov rax, [rsp-8] ; ret ; end: ret
        head = STORE_RSI_M8 + h('83 f9 01 77 1e 48 8d 15 1c 00 00 00 48 63 04 8a 48 01 d0 ff e0')
        case0 = STORE_EDI_PTR + LOAD_RAX_M8 + RET
        case1 = LOAD_RAX_M8 + RET
        code = head + case0 + case1 + RET
        start = 0x1000
        table_at = start + 17 + 0x1c                  # rip after the lea + 0x1c
        image = bytearray(bytes(start) + code + b'\xcc' * 64)
        image[table_at:table_at + 8] = struct.pack('<ii', start + len(head) - table_at,
                                                   start + len(head) + len(case0) - table_at)
        f = red_zone.decode_function(bytes(image), start, start + len(code), InstructionInfoFactory())
        self.assertEqual(f.tables, {start + len(head) - 2: [start + len(head), start + len(head) + len(case0)]})
        self.assertFalse(f.conservative)
        red_zone.liveness(f)
        self.assertTrue(f.ops[start + len(head) - 2].live & red_zone.RZ_MASK)   # live across the jmp rax


@unittest.skipUnless(red_zone.iced_x86, 'pip install iced-x86')
class EhFrameTests(unittest.TestCase):
    def test_reads_function_bounds_from_eh_frame_hdr(self):
        image = bytearray(0x3000)
        cie, fde, hdr = 0x2000, 0x2020, 0x2100
        image[cie:cie + 20] = struct.pack('<Ii', 16, 0) + b'\x01zR\x00\x01\x78\x10\x01\x1b' + bytes(3)
        for i, (begin, length) in enumerate(((0x100, 0x40), (0x200, 0x80))):
            at = fde + 24 * i
            image[at:at + 24] = struct.pack('<Ii', 20, at + 4 - cie) + struct.pack('<iI', begin - (at + 8), length) \
                + bytes(8)
        image[hdr:hdr + 12] = b'\x01\x1b\x03\x3b' + struct.pack('<iI', 0, 2)
        image[hdr + 12:hdr + 28] = struct.pack('<4i', 0x100 - hdr, fde - hdr, 0x200 - hdr, fde + 24 - hdr)
        functions = red_zone.find_functions(bytes(image), [(0, 0x1000, 5), (0x1000, 0x2000, 4)])
        self.assertEqual(functions, [(0x100, 0x140), (0x200, 0x280)])

    def test_table_outside_code_is_ignored(self):
        image = bytearray(0x3000)
        image[0x2100:0x2110] = b'\x01\x1b\x03\x3b' + struct.pack('<iIii', 0, 1, 0x2800 - 0x2100, 0)
        self.assertEqual(red_zone.find_functions(bytes(image), [(0, 0x1000, 5), (0x1000, 0x2000, 4)]), [])


@unittest.skipUnless(red_zone.iced_x86, 'pip install iced-x86')
class FixtureTests(unittest.TestCase):
    def test_fixture_files_are_current(self):
        image, _, _ = fixture()
        table, _, _ = fixture_table()
        self.assertEqual(FIXTURE_IMAGE.read_bytes(), image, 'python3 tests/test_red_zone.py --write')
        self.assertEqual(FIXTURE_TABLE.read_bytes(), table, 'python3 tests/test_red_zone.py --write')

    def test_table_header_carries_the_section(self):
        table, builder, stats = fixture_table()
        header = isa_rewrite.HEADER.unpack_from(table, 0)
        self.assertEqual((header[3], header[5]), (0, len(table) - isa_rewrite.HEADER.size))
        magic, code_offset, code_bytes, groups, group_bytes, version, crc = red_zone.SECTION.unpack_from(table, 64)
        self.assertEqual((magic, code_offset, groups, version), (red_zone.MAGIC, IMAGE_SIZE, len(builder.groups), 1))
        self.assertEqual(stats['protected'], 9)

    def test_cached_table_is_kept_only_for_the_same_image_and_scripts(self):
        import hashlib
        import tempfile
        from pathlib import Path
        image, _, _ = fixture()
        table, _, _ = fixture_table()
        current = bytearray(table)
        current[64 + 32:64 + 36] = struct.pack('<I', red_zone.source_crc())
        digest = hashlib.sha256(image).digest()
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / 'isa-rewrite.bin'
            path.write_bytes(bytes(current))
            self.assertIn('table kept', isa_rewrite.cached(path, digest, len(image), True))
            self.assertIsNone(isa_rewrite.cached(path, digest, len(image), False))      # options differ
            self.assertIsNone(isa_rewrite.cached(path, bytes(32), len(image), True))   # another image
            path.write_bytes(table)                                                      # other scripts (CRC 0)
            self.assertIsNone(isa_rewrite.cached(path, digest, len(image), True))


if __name__ == '__main__':
    if sys.argv[1:] == ['--write']:
        image, _, _ = fixture()
        table, _, stats = fixture_table()
        FIXTURE_IMAGE.write_bytes(image)
        FIXTURE_TABLE.write_bytes(table)
        print(red_zone.summary(stats))
    else:
        unittest.main()
