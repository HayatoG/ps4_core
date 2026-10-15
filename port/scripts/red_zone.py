"""Windows red-zone protection for guest code (iss_pc #29, #41; "disc is corrupt" #28, #30).

PS4 code follows the SysV ABI: a leaf function may keep data in the 128 bytes below rsp (the red
zone) without moving rsp. Windows has no red zone. When a guest thread faults (the GPU page tracking
write- and read-protects guest pages thousands of times per second), the kernel writes the exception
frame (CONTEXT, EXCEPTION_RECORD, ...) right below the faulting rsp and the vectored handler then runs
on that stack, so the red zone is overwritten. Linux skips 128 bytes before the signal frame.

The design follows shadPS4's "Windows static guest red-zone protection" (src/core/cpu_patches.cpp,
upstream commit 15abf68, GPL-2.0-or-later, by the shadPS4 Emulator Project; PRs #4802 and #5216),
rewritten here in Python on iced-x86 for our load-time table:

1. Function bounds come from the .eh_frame_hdr search tables in the linked image (eboot, libc and
   the other linked modules), each FDE giving [pc_begin, pc_begin + pc_range).
2. A function is decoded by following its control flow from the start (direct branches; bounded PIC
   jump tables `lea/movsxd/add/jmp reg` are resolved). Unresolved indirect jumps, branch targets that
   fall inside an instruction and red-zone pointers that escape (`lea reg, [rsp-k]`, index registers)
   make the function "conservative": the red zone counts as live everywhere in it.
3. A backward liveness pass over the bytes below rsp ([rsp-128, rsp), tracked per byte) follows
   rsp through push/pop/add/sub/lea; unlike upstream it also tracks [rsp, rsp+4096) so a value
   stored in the red zone before `sub rsp, n` and read at a positive offset afterwards stays live,
   and the targets of a resolved jump table are successors of its jmp.
4. Every instruction that can fault on a tracked page (a memory access not based on rsp; fs/gs
   TLS accesses excluded) while some red-zone byte is live is moved out of line (an ISA-rewrite
   site too: the loader applies the emulation first, and a group whose bytes changed is skipped): the site becomes
   `jmp stub` and the stub runs `lea rsp, [rsp-128]; <instruction>; lea rsp, [rsp+128]`, then jumps
   back. A fault there leaves rsp 128 bytes lower, so Windows writes its frame below the red zone,
   however deep the handler goes. lea does not touch the flags; no register changes.
5. A site shorter than 5 bytes takes the next instructions (or the previous ones) with it, never
   across a branch target, a call or before a terminator; failing that, a 2-byte `jmp short` reaches a
   relay `jmp near` placed in the inter-function padding or in the nops of an earlier patch.

The stub code is encoded here (iced BlockEncoder, which re-targets RIP-relative operands and
branches) for the address the loader gives the stub area: right after the image and the emulation
stubs (code_offset below). The loader copies it and writes each group's patches only when every
original byte still matches (a game patch or hook changed nothing there).
"""
from dataclasses import dataclass, field
from pathlib import Path
import struct
import zlib

try:
    import iced_x86
    from iced_x86 import (BlockEncoder, Code, Decoder, FlowControl, Instruction, InstructionInfoFactory,
                          MemoryOperand, MemorySizeExt, Mnemonic, OpAccess, OpKind, Register, RegisterExt)
except ImportError:
    iced_x86 = None

RED_ZONE = 128
ABOVE = 4096                      # bytes above rsp tracked as well (see 3. above)
BITS = RED_ZONE + ABOVE
ALL = (1 << BITS) - 1
RZ_MASK = (1 << RED_ZONE) - 1     # bits 0..127 = offsets -128..-1
NEAR, SHORT = 5, 2
PAGE = 4096
MAGIC = b'G3RZONE1'
VERSION = 1                        # section format (checked by the loader)
# magic, code_offset, code bytes, groups, group bytes, version, CRC-32 of the analysis sources (a
# table cached by isa_rewrite.py is rebuilt when this script or isa_rewrite.py changes)
SECTION = struct.Struct('<8sQIIIII')
STATS_FIELDS = ('functions', 'red_zone_functions', 'conservative_functions', 'candidates', 'protected',
                'groups', 'short_relays', 'skipped_stack', 'skipped_control', 'skipped_isa',
                'skipped_unrelocatable')
STATS = struct.Struct('<' + 'I' * len(STATS_FIELDS))
GROUP = struct.Struct('<QIIHH')   # site (first protected instruction), stub offset, stub bytes, protected, patches
PATCH = struct.Struct('<QH')      # image offset, length; then original and new bytes

READS = (1, 2, 5, 6) if iced_x86 is None else (OpAccess.READ, OpAccess.COND_READ, OpAccess.READ_WRITE,
                                               OpAccess.READ_COND_WRITE)
WRITES = (3, 5) if iced_x86 is None else (OpAccess.WRITE, OpAccess.READ_WRITE)
ANY_WRITE = (3, 4, 5, 6) if iced_x86 is None else (OpAccess.WRITE, OpAccess.COND_WRITE, OpAccess.READ_WRITE,
                                                   OpAccess.READ_COND_WRITE)


def signed64(v):
    return v - (1 << 64) if v >= 1 << 63 else v


def full(reg):
    return RegisterExt.full_register(reg) if reg != Register.NONE else Register.NONE


# ---- function bounds (.eh_frame_hdr) ----

def _uleb(data, pos):
    value = shift = 0
    while True:
        b = data[pos]
        pos += 1
        value |= (b & 0x7f) << shift
        shift += 7
        if b < 0x80:
            return value, pos


def _read_encoded(data, pos, encoding):
    """DW_EH_PE value at pos (pcrel/absptr, data and text relative bases are not used by clang here)."""
    if encoding == 0xff:
        return None, pos
    fmt = {0x00: '<Q', 0x02: '<H', 0x03: '<I', 0x04: '<Q', 0x0a: '<h', 0x0b: '<i', 0x0c: '<q'}.get(encoding & 0x0f)
    if fmt is None:
        raise ValueError(f'pointer encoding {encoding:#x}')
    value, = struct.unpack_from(fmt, data, pos)
    if encoding & 0x70 == 0x10:
        value += pos
    elif encoding & 0x70 not in (0x00,):
        raise ValueError(f'pointer encoding {encoding:#x}')
    return value, pos + struct.calcsize(fmt)


def _fde_encoding(image, cie):
    """FDE pointer encoding from the CIE's augmentation ('zR...')."""
    pos = cie + 8
    version = image[pos]
    pos += 1
    end = image.index(b'\0', pos)
    augmentation = bytes(image[pos:end])
    pos = end + 1
    _, pos = _uleb(image, pos)                 # code alignment
    _, pos = _uleb(image, pos)                 # data alignment (sleb, skipped as uleb bytes)
    if version == 1:
        pos += 1
    else:
        _, pos = _uleb(image, pos)
    encoding = 0
    if augmentation.startswith(b'z'):
        _, pos = _uleb(image, pos)
        for c in augmentation[1:]:
            if c == ord('R'):
                encoding = image[pos]
                pos += 1
            elif c == ord('P'):
                enc = image[pos]
                _, pos = _read_encoded(image, pos + 1, enc)
            elif c == ord('L'):
                pos += 1
            elif c in (ord('S'), ord('B')):
                pass
            else:
                break
    return encoding


def find_functions(image, segments):
    """(start, end) of every FDE found through .eh_frame_hdr search tables (version 1, eh_frame_ptr
    pcrel sdata4, fde_count udata4, table datarel sdata4), sorted. Each table is checked: every
    location inside an executable segment, ascending."""
    executable = [(a, a + s) for a, s, f in segments if f & 1]

    def in_code(x):
        return any(a <= x < b for a, b in executable)

    functions = []
    for a, size, flags in segments:
        start = 0
        segment = image[a:a + size]
        while True:
            at = segment.find(b'\x01\x1b\x03\x3b', start)
            if at < 0:
                break
            start = at + 1
            hdr = a + at
            count, = struct.unpack_from('<I', image, hdr + 8)
            if not 0 < count < 1 << 22 or hdr + 12 + 8 * count > len(image):
                continue
            table = struct.unpack_from(f'<{2 * count}i', image, hdr + 12)
            locations = [hdr + table[2 * i] for i in range(count)]
            if any(not in_code(x) for x in locations) or any(x > y for x, y in zip(locations, locations[1:])):
                continue
            try:
                for i in range(count):
                    fde = hdr + table[2 * i + 1]
                    length, cie_delta = struct.unpack_from('<Ii', image, fde)
                    if length in (0, 0xffffffff) or cie_delta == 0:
                        raise ValueError('bad FDE')
                    encoding = _fde_encoding(image, fde + 4 - cie_delta)
                    begin, pos = _read_encoded(image, fde + 8, encoding)
                    span, _ = _read_encoded(image, pos, encoding & 0x0f)
                    if begin != locations[i] or not span or not in_code(begin):
                        raise ValueError('FDE does not match its table entry')
                    functions.append((begin, begin + span))
            except (ValueError, IndexError, struct.error):
                continue
    functions = sorted(set(functions))
    # Overlapping entries (aliases) keep the first; a function ends at the next start at the latest.
    bounded = []
    for i, (s, e) in enumerate(functions):
        if bounded and s < bounded[-1][1]:
            continue
        if i + 1 < len(functions):
            e = min(e, functions[i + 1][0]) if functions[i + 1][0] > s else e
        bounded.append((s, e))
    return bounded


# ---- decoding and per-instruction facts ----

@dataclass(eq=False)
class Op:
    ins: object
    ip: int
    end: int
    use: int = 0
    dfn: int = 0
    live: int = 0
    faultable: bool = False   # memory access that can hit a tracked page
    uses_sp: bool = False
    delta: object = None      # known rsp change, None if none
    sp_unknown: bool = False
    rz: bool = False          # explicit [rsp - k] operand
    unmodeled: bool = False   # red-zone pointer escapes or unknown address
    flow: int = 0
    target: int = 0           # direct branch target, 0 if none
    successors: tuple = ()


TERMINATORS = None if iced_x86 is None else (FlowControl.UNCONDITIONAL_BRANCH, FlowControl.INDIRECT_BRANCH,
                                             FlowControl.RETURN, FlowControl.INTERRUPT, FlowControl.EXCEPTION)


def _mask(lo, hi):
    """Bits of the offsets [lo, hi) relative to rsp, clipped to the tracked window."""
    lo, hi = max(lo, -RED_ZONE), min(hi, ABOVE)
    return ((1 << (hi - lo)) - 1) << (lo + RED_ZONE) if hi > lo else 0


def describe(ins, factory):
    op = Op(ins, ins.ip, ins.next_ip, flow=ins.flow_control)
    info = factory.info(ins)
    writes_sp = False
    for r in info.used_registers():
        if full(r.register) == Register.RSP:
            op.uses_sp = True
            writes_sp |= r.access in ANY_WRITE
    for m in info.used_memory():
        if m.access == OpAccess.NO_MEM_ACCESS:
            continue
        if full(m.base) == Register.RSP or full(m.index) == Register.RSP:
            continue
        if m.segment in (Register.FS, Register.GS):
            continue
        op.faultable = True
    if ins.op_count and ins.flow_control in (FlowControl.UNCONDITIONAL_BRANCH, FlowControl.CONDITIONAL_BRANCH,
                                             FlowControl.CALL) and ins.op0_kind == OpKind.NEAR_BRANCH64:
        op.target = ins.near_branch_target
    m = ins.mnemonic
    explicit_sp = any(ins.op_kind(i) == OpKind.REGISTER and full(ins.op_register(i)) == Register.RSP
                      for i in range(ins.op_count))
    if op.flow in (FlowControl.CALL, FlowControl.INDIRECT_CALL, FlowControl.RETURN):
        pass                                   # a call returns with rsp unchanged; ret ends the path
    elif m in (Mnemonic.ADD, Mnemonic.SUB) and ins.op_count == 2 and ins.op0_kind == OpKind.REGISTER \
            and ins.op0_register == Register.RSP and ins.op1_kind in (OpKind.IMMEDIATE8TO64, OpKind.IMMEDIATE32TO64):
        imm = signed64(ins.immediate(1))
        op.delta = imm if m == Mnemonic.ADD else -imm
    elif m == Mnemonic.LEA and ins.op0_register == Register.RSP and ins.memory_base == Register.RSP \
            and ins.memory_index == Register.NONE:
        op.delta = signed64(ins.memory_displacement)
    elif ins.stack_pointer_increment and not explicit_sp and m not in (Mnemonic.ENTER, Mnemonic.LEAVE):
        op.delta = ins.stack_pointer_increment
    elif writes_sp:
        op.sp_unknown = True
    for i in range(ins.op_count):
        if ins.op_kind(i) != OpKind.MEMORY or full(ins.memory_base) != Register.RSP:
            continue
        disp = signed64(ins.memory_displacement)
        if m == Mnemonic.LEA:
            if op.delta is None and disp < 0:
                op.rz = op.unmodeled = True    # a pointer into the red zone
            continue
        if disp < 0:
            op.rz = True
        if ins.memory_index != Register.NONE:
            op.unmodeled |= disp < ABOVE        # any byte near rsp
            continue
        size = max(MemorySizeExt.size(ins.memory_size), 1)
        bits = _mask(disp, disp + size)
        access = info.op_access(i)
        if access in READS:
            op.use |= bits
        if access in WRITES:
            op.dfn |= bits
    return op


@dataclass
class Function:
    start: int
    end: int
    ops: dict = field(default_factory=dict)   # ip -> Op
    targets: set = field(default_factory=set)
    tables: dict = field(default_factory=dict)  # jmp ip -> resolved targets
    indirect: set = field(default_factory=set)
    uses_red_zone: bool = False
    conservative: bool = False


def _decode_at(image, ip, end):
    return Decoder(64, image[ip:end], ip=ip).decode()


def _previous(function, ip):
    """The decoded instruction that ends exactly at ip."""
    for length in range(1, 16):
        op = function.ops.get(ip - length)
        if op is not None and op.end == ip:
            return op
    return None


def _writes(op, reg, factory):
    return any(full(r.register) == reg and r.access in ANY_WRITE for r in factory.info(op.ins).used_registers())


def resolve_jump_table(function, jmp, image, factory):
    """Targets of `movsxd t, [base + index*4]; add t, base; jmp t` bounded by `cmp index, n; ja/jae`
    and `lea base, [rip + table]` (shadPS4 ResolveBoundedJumpTable); None when the pattern differs."""
    ins = jmp.ins
    if ins.op_count != 1 or ins.op0_kind != OpKind.REGISTER:
        return None
    target_reg = full(ins.op0_register)
    cursor, add = jmp, None
    for _ in range(5):
        prev = _previous(function, cursor.ip)
        if prev is None:
            return None
        p = prev.ins
        if p.mnemonic == Mnemonic.ADD and p.op0_kind == OpKind.REGISTER and full(p.op0_register) == target_reg \
                and p.op1_kind == OpKind.REGISTER:
            add = prev
            break
        if _writes(prev, target_reg, factory) or prev.flow in TERMINATORS:
            return None
        cursor = prev
    if add is None:
        return None
    table_reg = full(add.ins.op1_register)
    load = _previous(function, add.ip)
    if load is None:
        return None
    li = load.ins
    if li.mnemonic != Mnemonic.MOVSXD or li.op0_kind != OpKind.REGISTER or full(li.op0_register) != target_reg \
            or li.op1_kind != OpKind.MEMORY or full(li.memory_base) != table_reg or li.memory_index == Register.NONE \
            or li.memory_index_scale != 4:
        return None
    index_reg = full(li.memory_index)
    size = guarded = None
    cursor = load
    for _ in range(64):
        prev = _previous(function, cursor.ip)
        if prev is None:
            break
        p = prev.ins
        if p.mnemonic == Mnemonic.CMP and p.op0_kind == OpKind.REGISTER and full(p.op0_register) == index_reg \
                and p.op1_kind in (OpKind.IMMEDIATE8, OpKind.IMMEDIATE32, OpKind.IMMEDIATE8TO32,
                                   OpKind.IMMEDIATE8TO64, OpKind.IMMEDIATE32TO64):
            branch = function.ops.get(prev.end)
            while branch is not None and branch.flow == FlowControl.NEXT and not branch.ins.rflags_modified:
                branch = function.ops.get(branch.end)    # e.g. a load between the cmp and the ja
            bound = signed64(p.immediate(1))
            if branch is None or bound < 0:
                return None
            if branch.ins.mnemonic == Mnemonic.JA:
                size = bound + 1
            elif branch.ins.mnemonic == Mnemonic.JAE:
                size = bound
            else:
                return None
            guarded = branch.end
            break
        if _writes(prev, index_reg, factory):
            return None
        cursor = prev
    if not size or size > 4096 or any(guarded <= t <= jmp.ip for t in function.targets):
        return None
    candidates = set()
    cursor = load
    for _ in range(64):
        prev = _previous(function, cursor.ip)
        if prev is None:
            break
        if _writes(prev, table_reg, factory):
            p = prev.ins
            if p.mnemonic == Mnemonic.LEA and p.is_ip_rel_memory_operand:
                candidates.add(p.ip_rel_memory_address)
            break
        cursor = prev
    if not candidates:
        for op in function.ops.values():
            p = op.ins
            if op.ip < jmp.ip and p.mnemonic == Mnemonic.LEA and p.op0_kind == OpKind.REGISTER \
                    and full(p.op0_register) == table_reg and p.is_ip_rel_memory_operand:
                candidates.add(p.ip_rel_memory_address)
    resolved = None
    for table in candidates:
        if table < 0 or table + 4 * size > len(image):
            continue
        entries = struct.unpack_from(f'<{size}i', image, table)
        targets = sorted({table + e for e in entries})
        if any(not function.start <= t < function.end for t in targets):
            continue
        if resolved is not None:
            return None
        resolved = targets
    return resolved


def decode_function(image, start, end, factory):
    f = Function(start, end)
    work = [start]
    while True:
        while work:
            ip = work.pop()
            while start <= ip < end and ip not in f.ops:
                ins = _decode_at(image, ip, end)
                if ins.code == Code.INVALID:
                    f.conservative = True
                    break
                op = describe(ins, factory)
                f.ops[ip] = op
                f.uses_red_zone |= op.rz
                f.conservative |= op.unmodeled
                if op.target and start <= op.target < end:
                    f.targets.add(op.target)
                if op.flow == FlowControl.CONDITIONAL_BRANCH:
                    if op.target and start <= op.target < end:
                        work.append(op.target)
                elif op.flow in TERMINATORS:
                    if op.flow == FlowControl.UNCONDITIONAL_BRANCH and start <= op.target < end:
                        work.append(op.target)
                    elif op.flow == FlowControl.INDIRECT_BRANCH:
                        f.indirect.add(ip)
                    break
                ip = op.end
        found = False
        for jmp in sorted(f.indirect - set(f.tables)):
            targets = resolve_jump_table(f, f.ops[jmp], image, factory)
            if targets is None:
                continue
            f.tables[jmp] = targets
            for t in targets:
                f.targets.add(t)
                if t not in f.ops:
                    work.append(t)
                    found = True
        if not found:
            break
    if f.indirect - set(f.tables):
        f.conservative = True
    if any(t not in f.ops for t in f.targets):
        f.conservative = True             # a branch into the middle of an instruction
    return f


def translate(mask, delta):
    """Live bits after an instruction that adds delta to rsp, in the coordinates before it."""
    return (mask << delta) & ALL if delta >= 0 else mask >> -delta


def liveness(f):
    ops = sorted(f.ops.values(), key=lambda o: o.ip)
    if f.conservative:
        for op in ops:
            op.live = ALL
        return
    for op in ops:
        if op.flow == FlowControl.CONDITIONAL_BRANCH:
            op.successors = (op.end, op.target)
        elif op.flow == FlowControl.UNCONDITIONAL_BRANCH:
            op.successors = (op.target,)
        elif op.flow == FlowControl.INDIRECT_BRANCH:
            op.successors = tuple(f.tables.get(op.ip, ()))
        elif op.flow in TERMINATORS:
            op.successors = ()
        else:
            op.successors = (op.end,)
    changed = True
    while changed:
        changed = False
        for op in reversed(ops):
            out = 0
            for s in op.successors:
                succ = f.ops.get(s)
                if succ is not None:
                    out |= succ.live
            if op.delta is not None:
                out = translate(out, op.delta)
            elif op.sp_unknown and out:
                out = ALL                    # rsp moves by an unknown amount: anything may be live
            new = op.use | (out & ~op.dfn)
            if new != op.live:
                op.live = new
                changed = True


# ---- sites, spans and code ----

def _plain_neighbour(op, isa_bytes):
    """May be moved into a stub next to a site: decoded, not a call (its return address would be the
    stub), no fs/gs (the loader patches TLS loads on Windows), no ISA-rewrite site bytes."""
    ins = op.ins
    if op.flow in (FlowControl.CALL, FlowControl.INDIRECT_CALL, FlowControl.XBEGIN_XABORT_XEND):
        return False
    if ins.segment_prefix in (Register.FS, Register.GS):
        return False
    return not any(b in isa_bytes for b in range(op.ip, op.end))


def _lea(delta):
    return Instruction.create_reg_mem(Code.LEA_R64_M, Register.RSP, MemoryOperand(Register.RSP, displ=delta))


class Builder:
    def __init__(self, image, code_offset):
        self.image = image
        self.code_offset = code_offset
        self.code = bytearray()
        self.groups = []        # (site, stub offset, stub bytes, protected, [(offset, original, new)])

    def stub(self, ops, protect, continuation):
        """Encodes ops (Op list) for the next free address; returns (offset, bytes) or None."""
        at = self.code_offset + len(self.code)
        instructions = []
        for op in ops:
            if op.ip in protect:
                instructions.append(_lea(-RED_ZONE))
            instructions.append(op.ins)
            if op.ip in protect:
                instructions.append(_lea(RED_ZONE))
        if not ops or ops[-1].flow not in TERMINATORS:
            instructions.append(Instruction.create_branch(Code.JMP_REL32_64, continuation))
        encoder = BlockEncoder(64)
        encoder.add_many(instructions)
        try:
            data = encoder.encode(at)
        except ValueError:
            return None
        start = len(self.code)
        self.code += data
        self.code += b'\xcc' * (-len(self.code) % 16)
        return start, len(data)

    def jmp_bytes(self, at, target, short=False):
        if short:
            rel = target - (at + SHORT)
            assert -128 <= rel <= 127
            return bytes([0xeb, rel & 0xff])
        return b'\xe9' + struct.pack('<i', target - (at + NEAR))


def plan_function(f, builder, isa_bytes, used, padding, stats):
    """Protects the faultable instructions of f where the red zone is live (see the module doc)."""
    image = builder.image
    sites = []
    for op in sorted(f.ops.values(), key=lambda o: o.ip):
        if not op.faultable or not (op.live & RZ_MASK):
            continue
        stats['candidates'] += 1
        if any(b in isa_bytes for b in range(op.ip, op.end)):
            # An ISA-rewrite site: when the loader emulates it, its bytes change first and this group
            # is skipped (the emulation stub runs 256 bytes below rsp); when the CPU has the
            # instruction, the group applies.
            stats['skipped_isa'] += 1
        if op.uses_sp:
            stats['skipped_stack'] += 1         # push/pop [mem], call [mem], ... would see the moved rsp
        elif op.flow != FlowControl.NEXT or op.ins.segment_prefix in (Register.FS, Register.GS):
            stats['skipped_control'] += 1
        else:
            sites.append(op)
    if not sites:
        return
    site_ips = {op.ip for op in sites}
    neighbours_ok = not (f.indirect - set(f.tables))
    covered_until = f.start
    relays = []        # free 5-byte slots inside earlier patches of this function
    unresolved = []

    def free(lo, hi):
        return not any(b in used for b in range(lo, hi))

    def emit(span, continuation):
        protect = {op.ip for op in span if op.ip in site_ips}
        stub = builder.stub(span, protect, continuation)
        if stub is None:
            return None
        return stub, protect

    def forward(site):
        span, size, ip = [], 0, site.ip
        while size < NEAR:
            op = f.ops.get(ip)
            if op is None or (ip != site.ip and (ip in f.targets or not _plain_neighbour(op, isa_bytes))):
                return None
            span.append(op)
            size += op.end - op.ip
            ip = op.end
            if size < NEAR and op.flow in TERMINATORS:
                return None
        return span

    def backward(site):
        span, start = [site], site.ip
        size = site.end - site.ip
        while size < NEAR:
            if start in f.targets:
                return None
            prev = _previous(f, start)
            if prev is None or prev.ip < covered_until or prev.flow in TERMINATORS \
                    or prev.flow == FlowControl.CONDITIONAL_BRANCH or not _plain_neighbour(prev, isa_bytes):
                return None
            span.insert(0, prev)
            start = prev.ip
            size += prev.end - prev.ip
        return span

    for site in sites:
        if site.ip < covered_until:
            continue
        chosen = None
        for collect in (forward, backward):
            if collect is backward and not neighbours_ok:
                break
            if collect is forward and not neighbours_ok and site.end - site.ip < NEAR:
                continue
            span = collect(site)
            if span is None or not free(span[0].ip, span[-1].end):
                continue
            made = emit(span, span[-1].end)
            if made:
                chosen = span, made
                break
        if chosen is None:
            unresolved.append(site)
            continue
        span, ((stub_at, stub_len), protect) = chosen
        lo, hi = span[0].ip, span[-1].end
        new = builder.jmp_bytes(lo, builder.code_offset + stub_at) + b'\x90' * (hi - lo - NEAR)
        builder.groups.append((site.ip, stub_at, stub_len, len(protect), [(lo, bytes(image[lo:hi]), new)]))
        used.update(range(lo, hi))
        if hi - lo >= 2 * NEAR:
            relays.append(lo + NEAR)
        covered_until = hi
        stats['protected'] += len(protect)
    def host_relay(site):
        """shadPS4's last fallback: a nearby run of plain instructions (10+ bytes) moves to its own
        stub, and its second 5 bytes become the relay `jmp near` of the site's `jmp short`."""
        ops = sorted(f.ops)
        for start in ops:
            relay = start + NEAR
            if not -128 <= relay - (site.ip + SHORT) <= 127:
                continue
            span, size, ip = [], 0, start
            while size < 2 * NEAR:
                op = f.ops.get(ip)
                if op is None or (ip != start and ip in f.targets) or not _plain_neighbour(op, isa_bytes) \
                        or (size + op.end - op.ip < 2 * NEAR and op.flow in TERMINATORS):
                    span = None
                    break
                span.append(op)
                size += op.end - op.ip
                ip = op.end
            if not span:
                continue
            lo, hi = span[0].ip, span[-1].end
            if not (hi <= site.ip or lo >= site.end) or not free(lo, hi):
                continue
            host = emit(span, hi)
            if host is None:
                continue
            own = emit([site], site.end)
            if own is None:
                return False
            (host_at, host_len), host_protect = host
            (stub_at, stub_len), _ = own
            new = (builder.jmp_bytes(lo, builder.code_offset + host_at)
                   + builder.jmp_bytes(relay, builder.code_offset + stub_at) + b'\x90' * (hi - lo - 2 * NEAR))
            patches = [(lo, bytes(image[lo:hi]), new),
                       (site.ip, bytes(image[site.ip:site.end]),
                        builder.jmp_bytes(site.ip, relay, short=True) + b'\x90' * (site.end - site.ip - SHORT))]
            # The group's stub range covers both stubs (emitted one after the other).
            builder.groups.append((site.ip, host_at, stub_at + stub_len - host_at, 1 + len(host_protect),
                                   sorted(patches)))
            used.update(range(lo, hi))
            used.update(range(site.ip, site.end))
            if hi - lo >= 3 * NEAR:
                relays.append(lo + 2 * NEAR)
            stats['protected'] += 1 + len(host_protect)
            stats['short_relays'] += 1
            return True
        return False

    # 2-byte jmp short to a relay (inter-function padding, or the nops of a patch above).
    for site in unresolved:
        if not free(site.ip, site.end):
            continue                            # taken into a later span as a neighbour
        length = site.end - site.ip
        slot = None
        for candidate in relays + padding:
            rel = candidate - (site.ip + SHORT)
            if -128 <= rel <= 127 and free(candidate, candidate + NEAR):
                slot = candidate
                break
        if slot is None:
            if neighbours_ok and host_relay(site):
                continue
            stats['skipped_unrelocatable'] += 1
            continue
        made = emit([site], site.end)
        if not made:
            stats['skipped_unrelocatable'] += 1
            continue
        (stub_at, stub_len), protect = made
        patches = [(site.ip, bytes(image[site.ip:site.end]),
                    builder.jmp_bytes(site.ip, slot, short=True) + b'\x90' * (length - SHORT)),
                   (slot, bytes(image[slot:slot + NEAR]), builder.jmp_bytes(slot, builder.code_offset + stub_at))]
        builder.groups.append((site.ip, stub_at, stub_len, 1, patches))
        used.update(range(site.ip, site.end))
        used.update(range(slot, slot + NEAR))
        if slot in relays:
            relays.remove(slot)
        else:
            padding.remove(slot)
        stats['protected'] += 1
        stats['short_relays'] += 1


def padding_slots(image, functions):
    """5-byte slots in the bytes between functions that are only nop/int3 padding (never executed)."""
    slots = []
    for (s, e), (n, _) in zip(functions, functions[1:]):
        gap = image[e:n]
        if len(gap) >= NEAR and set(gap) <= {0x90, 0xcc}:
            for at in range(e, n - NEAR + 1, NEAR):
                slots.append(at)
    return slots


def analyze(image, segments, code_offset, isa_bytes=frozenset(), functions=None):
    """Groups and stub code protecting the red zone of every function of the linked image
    (functions: (start, end) list, by default from the image's .eh_frame_hdr tables)."""
    if iced_x86 is None:
        raise RuntimeError('iced_x86 is not installed')
    stats = dict.fromkeys(STATS_FIELDS, 0)
    functions = find_functions(image, segments) if functions is None else sorted(functions)
    stats['functions'] = len(functions)
    if not functions:
        raise ValueError('no .eh_frame_hdr function table in the image')
    factory = InstructionInfoFactory()
    builder = Builder(image, code_offset)
    padding = padding_slots(image, functions)
    used = set()
    for start, end in functions:
        # Cheap filter: only functions with an explicit [rsp - k] operand (not lea rsp) use the red zone.
        uses = False
        for ins in Decoder(64, image[start:end], ip=start):
            if ins.memory_base == Register.RSP and ins.memory_displacement >= 1 << 63 \
                    and not (ins.mnemonic == Mnemonic.LEA and ins.op0_register == Register.RSP):
                uses = True
                break
        if not uses:
            continue
        f = decode_function(image, start, end, factory)
        if not f.uses_red_zone:
            continue
        stats['red_zone_functions'] += 1
        stats['conservative_functions'] += f.conservative
        liveness(f)
        plan_function(f, builder, isa_bytes, used, padding, stats)
    stats['groups'] = len(builder.groups)
    return builder, stats


def source_crc():
    here = Path(__file__).resolve().parent
    return zlib.crc32(b''.join((here / name).read_bytes() for name in ('red_zone.py', 'isa_rewrite.py')))


def encode_section(builder, stats):
    groups = bytearray()
    for site, stub_at, stub_len, protected, patches in builder.groups:
        groups += GROUP.pack(site, stub_at, stub_len, protected, len(patches))
        for offset, original, new in patches:
            assert len(original) == len(new)
            groups += PATCH.pack(offset, len(original)) + original + new
    code = bytes(builder.code)
    return (SECTION.pack(MAGIC, builder.code_offset, len(code), len(builder.groups), len(groups), VERSION, source_crc())
            + STATS.pack(*(stats[k] for k in STATS_FIELDS)) + code + bytes(groups))


def summary(stats):
    return (f"Red zone: {stats['protected']} instructions protected in {stats['groups']} patches "
            f"({stats['short_relays']} through short-jump relays) over {stats['red_zone_functions']} red-zone functions "
            f"({stats['conservative_functions']} analysed conservatively) of {stats['functions']}; "
            f"{stats['candidates']} candidates ({stats['skipped_isa']} also ISA-rewrite sites), not protected: "
            f"{stats['skipped_stack']} use rsp, {stats['skipped_control']} control flow, "
            f"{stats['skipped_unrelocatable']} unrelocatable")


def code_offset_for(image_size, site_count):
    """Where the loader puts the stub code: after the image and the emulation stubs, page aligned."""
    def up(n):
        return (n + PAGE - 1) // PAGE * PAGE
    return up(image_size) + up(site_count * 256)
