#!/usr/bin/env python3
"""
lift.py - lift Epic Pinball to C.

    py tools/lift.py            # every image the layout knows
    py tools/lift.py pin ep1    # just these

Epic Pinball is not one program. PINBALL.EXE is the menu and the music host:
it loads the MASI sound system (MDRV000R.MUS, the mixer, plus one card driver
such as MDRV004R.MUS, Sound Blaster) into memory it allocates, then EXECs one
table, EP1.EXE .. EP13.EXE, passing it the address of its music interface on
the command line. The table calls back into PINBALL, which calls the driver.
All of it is hand-written assembly (TASM), so none of the Borland assumptions
Blake Stone's driver leans on hold here.

Each of those files is an *image*, lifted at the segment it is loaded at.
Where that is depends on what was allocated before it, so the runtime records
every placement in work/layout.txt (src/images.c) and this script reads it
back; an image the layout does not know yet is skipped with a note. The
runtime refuses to run an image at a segment other than the one it was lifted
at. Why: docs/architecture.md.

  1. discover    recursive descent from the entry point and every recorded
                 run-time miss, following calls and jumps by decoding them
  2. lift        pcrecomp lift16.Lifter, one C function per entry point
  3. emit        work/gen/<image>/: chunked C, prototypes, segment constants,
                 and the address -> function table indirect calls go through

The output is generated from the retail binaries and is never committed.
"""
import filecmp
import json
import re
import shutil
import os
import struct
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DATA = os.path.join(ROOT, 'original')
WORK = os.path.join(ROOT, 'work')

# name -> file. Every table is its own image; they share a load segment, so
# only one is ever live (src/images.c).
IMAGES = {'pin': 'PINBALL.EXE'}
IMAGES.update({f'ep{n}': f'EP{n}.EXE' for n in range(1, 14)})
IMAGES.update({'mlpr': 'MDRV000R.MUS', 'nosnd': 'MDRV001R.MUS', 'sb': 'MDRV004R.MUS'})

# PINBALL.EXE goes where DOS would put the first program on a clean machine.
# Baked into the lifted C; src/dos.c agrees (PSP_SEG).
PSP_SEG = 0x0100

NL = chr(10)
CHUNK = 1500         # instructions per generated .c file: MSVC is slow on huge TUs


def _pcrecomp_home():
    """PCRECOMP_HOME, else a sibling checkout -- never one person's drive."""
    env = os.environ.get('PCRECOMP_HOME')
    cands = [env] if env else []
    cands += [os.path.join(os.path.dirname(ROOT), 'pcrecomp'),
              os.path.join(os.path.dirname(ROOT), 'pcrecomp-main')]
    for c in cands:
        if c and os.path.isdir(os.path.join(c, 'tools', 'lift')):
            return os.path.abspath(c)
    sys.exit('cannot find the pcrecomp toolkit; set PCRECOMP_HOME to a checkout of\n'
             '  https://github.com/sp00nznet/pcrecomp')


PCR = _pcrecomp_home()
for sub in ('disasm', 'lift'):
    sys.path.insert(0, os.path.join(PCR, 'tools', sub))
import decode16                     # noqa: E402
decode16.WRAP_NEAR_TARGETS = True   # IP arithmetic wraps inside the segment
from decode16 import Decoder, OpType  # noqa: E402
import lift16                       # noqa: E402
from lift16 import Lifter           # noqa: E402

TERMINATORS = {'ret', 'retf', 'iret', 'hlt'}
JCC = {'jo', 'jno', 'jb', 'jae', 'je', 'jne', 'jbe', 'ja', 'js', 'jns', 'jp',
       'jnp', 'jl', 'jge', 'jle', 'jg', 'loop', 'loopz', 'loopnz', 'jcxz'}


def read_layout():
    """{image: load segment} as the runtime recorded it, plus PINBALL's own."""
    out = {'pin': PSP_SEG + 0x10}
    p = os.path.join(WORK, 'layout.txt')
    if os.path.exists(p):
        for line in open(p):
            f = line.split()
            if len(f) == 2 and f[0] in IMAGES:
                out[f[0]] = int(f[1], 16)
    return out


class Image:
    """An MZ program, or a raw MASI driver blob (code at offset 0)."""

    def __init__(self, name, path):
        raw = open(path, 'rb').read()
        self.name, self.raw = name, raw
        if raw[:2] == b'MZ':
            h = struct.unpack_from('<14H', raw, 0)
            size = h[2] * 512 - ((512 - h[1]) if h[1] else 0)
            self.img = raw[h[4] * 16:size]
            self.ss, self.sp, self.ip, self.cs = h[7], h[8], h[10], h[11]
            self.relocs = [struct.unpack_from('<HH', raw, h[12] + 4 * i) for i in range(h[3])]
            self.entries = [(self.cs, self.ip)]
        else:
            # A MASI driver: FE "REAL16" FE, name, then dword offsets of its
            # entry points at 28h (the function dispatcher) and 2Ch.
            self.img = raw
            self.ss = self.sp = self.ip = self.cs = 0
            self.relocs = []
            self.entries = []
            if raw[1:7] == b'REAL16':
                for at in (0x28, 0x2C):
                    v = struct.unpack_from('<I', raw, at)[0]
                    if 0 < v < len(raw):
                        self.entries.append((0, v))
        self.reloc_at = {s * 16 + o for o, s in self.relocs}
        self._dec = {}

    def decode(self, cs, ip):
        d = self._dec.get(cs)
        if d is None:
            d = self._dec[cs] = Decoder(self.img[cs * 16:cs * 16 + 0x10000], cs * 16)
        d.pos = ip
        try:
            return d.decode_one()
        except (IndexError, KeyError):
            return None


def jump_table(img, fn, ins):
    """Arms of `jmp word cs:[bx+table]` with a `cmp bx, N` bound in front, as
    IPs. Without a bound the jump goes to the runtime dispatcher instead."""
    o = ins.op1
    if not (o and o.type == OpType.MEM and o.base in ('bx', 'si', 'di') and not o.index
            and o.seg == 'cs'):
        return None
    prev = [fn[a] for a in sorted(fn) if ins.address - 24 <= a < ins.address]
    bound = None
    for j in prev[-8:]:
        if (j.mnemonic == 'cmp' and j.op1 and j.op1.type == OpType.REG16
                and repr(j.op1) == o.base and j.op2 and j.op2.type in (OpType.IMM8, OpType.IMM16)):
            bound = (j.op2.disp & 0xFFFF) + 1
    if not bound or bound > 0x400:
        return None
    base = ins.offset - ins.address               # cs * 16
    start = o.disp & 0xFFFF
    return [struct.unpack_from('<H', img, base + ((start + 2 * k) & 0xFFFF))[0]
            for k in range(bound)]


def ptr_table(im, cs, ins):
    """Targets of `call/jmp word [bx+table]` with no bound in front: the run
    of words at the table that each land on decodable code in this segment.
    Only where the table's segment is known -- `cs:`, or `ds:` in a driver
    blob, whose entry sets DS = CS (MASI's `call [bx+279h]` function table)."""
    o = ins.op1
    if not (o and o.type == OpType.MEM and o.size == 2 and (
            (o.base in ('bx', 'si', 'di') and not o.index)
            or (not o.base and o.index and getattr(o, 'scale', 1) == 2))):   # [ebx*2+table]
        return None
    if not (o.seg == 'cs' or (not im.relocs and o.seg in ('', 'ds'))):
        return None
    lo, hi = cs * 16, min(cs * 16 + 0x10000, len(im.img))
    out = []
    for k in range(256):
        p = lo + ((o.disp + 2 * k) & 0xFFFF)
        # A table ends where the code it points into begins: the tables'
        # key handlers (`sub bx, 0AAh / shl bx, 1 / mov bx, cs:[bx+1FCBh] /
        # jmp bx`) sit right after their 86 entries, and reading on turned
        # the handlers' own bytes into arms into the graphics.
        if p + 2 > hi or (out and p >= lo + min(out)):
            break
        v = struct.unpack_from('<H', im.img, p)[0]
        if not (0x10 <= v < hi - lo) or lo + v in range(p - 2 * k, p + 2) or not plausible(im, cs, v, 3):
            break
        out.append(v)
    return out or None


def loaded_from(fn, ins):
    """For `jmp reg`, the `mov reg, word [base+table]` just before it, as an
    instruction whose op1 is that memory operand -- what ptr_table reads.
    The MASI mixer's command interpreter: `mov bx, [bx+84Dh] / jmp bx`."""
    if ins.op1 is None or ins.op1.type != OpType.REG16:
        return None
    r = repr(ins.op1)
    for a in sorted((a for a in fn if ins.address - 12 <= a < ins.address), reverse=True):
        j = fn[a]
        if j.op1 is not None and repr(j.op1) == r:
            if j.mnemonic == 'mov' and j.op2 is not None and j.op2.type == OpType.MEM:
                k = decode16.Instruction()
                k.__dict__.update(j.__dict__)
                k.op1, k.op2 = j.op2, None
                return k
            return None
    return None


def exits(fn, ins):
    """`mov ax, 4Cxx` / `mov ah, 4Ch` then `int 21h`: the program ends here.
    Hand-written asm puts its messages right after, and decoding on into
    them invents jumps into the middle of real instructions."""
    if ins.mnemonic != 'int' or ins.op1 is None or (ins.op1.disp & 0xFF) not in (0x20, 0x21):
        return False
    if (ins.op1.disp & 0xFF) == 0x20:
        return True
    prev = [j for j in fn.values() if j.address + j.length == ins.address and j.mnemonic == 'mov']
    for j in prev:
        r, v = repr(j.op1), j.op2
        if v is not None and v.type in (OpType.IMM8, OpType.IMM16):
            if (r == 'ax' and (v.disp >> 8) & 0xFF == 0x4C) or (r == 'ah' and v.disp & 0xFF == 0x4C):
                return True
    return False


def discover(im, extra):
    """{linear entry: cs}, {linear entry: {ip: Instruction}}, {entry: {jmp ip: arms}}"""
    entries, work = {}, []

    def add(cs, ip):
        lin = cs * 16 + ip
        if not 0 <= lin < len(im.img):
            return
        if lin not in entries:
            entries[lin] = cs
            work.append(lin)

    for cs, ip in im.entries:
        add(cs, ip)
    # Recorded entries wait until everything reachable by calls has been
    # walked: a call states its target's CS, a recorded miss only implies one.
    pending = sorted(extra)

    bodies, tables, bad = {}, {}, []
    while work or pending:
        if not work:
            for cs, ip in pending:
                add(cs, ip)
            pending = []
            continue
        lin = work.pop()
        cs = entries[lin]
        fn, tbl = {}, {}
        todo = [lin - cs * 16]
        while todo:
            ip = todo.pop()
            if ip in fn:
                continue
            ins = im.decode(cs, ip)
            if ins is None or ins.mnemonic == 'db':
                bad.append((cs, ip, lin))
                continue
            fn[ip] = ins
            m, o = ins.mnemonic, ins.op1
            nxt = (ip + ins.length) & 0xFFFF
            if m in TERMINATORS or m == 'jmp far' or exits(fn, ins):
                continue
            if m == 'jmp':
                if o.type in (OpType.REL8, OpType.REL16):
                    todo.append(o.disp & 0xFFFF)
                elif o.type == OpType.FAR:
                    add(o.far_seg, o.disp)
                else:
                    arms = (jump_table(im.img, fn, ins) or ptr_table(im, cs, loaded_from(fn, ins) or ins)
                            or unrolled_arms(im, cs, ins))
                    if arms:
                        tbl[ip] = arms
                        todo.extend(arms)
                continue
            if m in JCC:
                todo.append(o.disp & 0xFFFF)
            elif m == 'call' and o and o.type == OpType.REL16:
                add(cs, o.disp & 0xFFFF)
            elif m == 'call' and o and o.type == OpType.FAR:
                add(o.far_seg, o.disp)
            elif m == 'call' and o and o.type == OpType.MEM:
                for v in ptr_table(im, cs, ins) or ():
                    add(cs, v)
            todo.append(nxt)
        bodies[lin] = fn
        tables[lin] = tbl
    return entries, bodies, tables, bad


def after_exit(img, t):
    """Does linear `t` follow a ret/retf/iret/jmp -- where a routine starts?"""
    return t >= 3 and (img[t - 1] in (0xC3, 0xCB, 0xCF) or img[t - 3] in (0xC2, 0xCA, 0xE9)
                       or img[t - 2] == 0xEB)


def stored_pointers(im, entries, bodies):
    """Routines nothing calls directly, found where their address is stored:
    `mov word [x], imm` where the same `[x]` is somewhere the operand of
    `call word [x]` or `jmp word [x]`. The MASI drivers keep their current
    output routine in a variable this way (`mov [223Dh], 25B5h` ...
    `call [223Dh]`). The variable has to be called through: the SB driver
    also stores table addresses (`mov [115Bh], 16CCh`), and its tables decode
    as plausibly as code does. A relocated immediate is a segment."""
    called = set()
    for lin, body in bodies.items():
        for ins in body.values():
            o = ins.op1
            if ins.mnemonic in ('call', 'jmp') and o is not None and o.type in (OpType.MEM, OpType.MOFFS)                     and not o.base and not o.index and o.size == 2:
                called.add((entries[lin], o.seg or 'ds', o.disp & 0xFFFF))
    out = set()
    for lin, body in bodies.items():
        cs = entries[lin]
        for ins in body.values():
            o, d = ins.op2, ins.op1
            if ins.mnemonic != 'mov' or d is None or d.type not in (OpType.MEM, OpType.MOFFS)                     or d.base or d.index or (cs, d.seg or 'ds', d.disp & 0xFFFF) not in called:
                continue
            if o is None or o.type != OpType.IMM16 or (ins.offset + ins.length - 2) in im.reloc_at:
                continue
            v = o.disp & 0xFFFF
            if cs * 16 + v < len(im.img) and plausible(im, cs, v):
                out.add((cs, v))
    return out


def unrolled_runs(im, cs):
    """Tables of entry points into unrolled loops in segment cs, each a list
    of IPs. Hand-tuned mixers jump into the middle of a loop unrolled N times
    to do the last few iterations (the SB driver: `jmp ax`, ax from one of
    four 32-word tables at 1214h/1440h/16CCh/1958h, the one in use chosen at
    run time). A table like that is a run of at least 8 words in arithmetic
    progression (a fixed instruction-group size, 2..64 bytes apart), all
    landing on plausible code in the segment. Cached per segment."""
    cache = im.__dict__.setdefault('_runs', {})
    if cs in cache:
        return cache[cs]
    img, out = im.img, []
    lo = cs * 16
    hi = min(lo + 0x10000, len(img))
    w = [img[p] | img[p + 1] << 8 for p in range(lo, hi - 1)]
    p = 0
    while p + 16 < len(w):
        d = (w[p + 2] - w[p]) & 0xFFFF
        d = d - 0x10000 if d >= 0x8000 else d
        n = 1
        if 2 <= abs(d) <= 64:
            while p + 2 * (n + 1) < len(w) and (w[p + 2 * (n + 1)] - w[p + 2 * n]) & 0xFFFF == d & 0xFFFF:
                n += 1
        run = [w[p + 2 * k] for k in range(n + 1)]
        if n + 1 >= 8 and all(0 < v < hi - lo and plausible(im, cs, v, 3) for v in run):
            # the table's first word is often the loop's top, out of step
            v = w[p - 2] if p >= 2 else 0
            if min(run) - 4 * abs(d) <= v <= max(run) + 4 * abs(d) and plausible(im, cs, v, 3):
                run.append(v)
            out.append(run)
            p += 2 * (n + 1)
        else:
            p += 1
    cache[cs] = out
    return out


def unrolled_arms(im, cs, ins):
    """Possible targets of a `jmp reg` nothing in front of it explains: the
    unrolled-loop entry tables within 4K of it (a mixer keeps its loops next
    to the code that jumps into them). Lifted as switch arms, so the loops
    are gotos inside one function rather than a function per entry."""
    if ins.op1 is None or ins.op1.type != OpType.REG16:
        return None
    arms = [v for run in unrolled_runs(im, cs) for v in run if abs(v - ins.address) <= 0x1000]
    return arms or None


def plausible(im, cs, ip, n=8):
    """Do the next n instructions at cs:ip look like code? Padding after a
    routine (zeros, FFs) decodes too, as `add [bx+si], al` and friends."""
    for _ in range(n):
        ins = im.decode(cs, ip)
        if ins is None or ins.mnemonic == 'db' or ins.raw[:2] in (b'\x00\x00', b'\xff\xff'):
            return False
        if ins.mnemonic in TERMINATORS or ins.mnemonic in ('jmp', 'jmp far'):
            return True
        ip = (ip + ins.length) & 0xFFFF
    return True


WRITES = {'mov', 'add', 'sub', 'adc', 'sbb', 'and', 'or', 'xor', 'inc', 'dec',
          'not', 'neg', 'shl', 'sal', 'shr', 'sar', 'rol', 'ror', 'pop', 'xchg',
          'stosb', 'stosw'}


def smc_targets(entries, bodies):
    """Linear addresses the code writes through a constant `cs:` address --
    hand-written inner loops patch their own immediates this way."""
    out = set()
    for lin, body in bodies.items():
        cs = entries[lin]
        for ins in body.values():
            o = ins.op1
            if (ins.mnemonic in WRITES and o is not None and o.type in (OpType.MEM, OpType.MOFFS)
                    and o.seg == 'cs' and not o.base and not o.index):
                for k in range(o.size or 2):
                    out.add(cs * 16 + ((o.disp + k) & 0xFFFF))
    return out


def load_misses(name, load_seg):
    """work/misses/<image>.txt: seg:off the runtime jumped to with no lifted
    function, absolute; turned back into image-relative (cs, ip)."""
    out = set()
    p = os.path.join(WORK, 'misses', f'{name}.txt')
    if os.path.exists(p):
        for line in open(p):
            line = line.strip()
            if ':' in line:
                s, o = line.split(':')[:2]
                seg = int(s, 16) - load_seg
                if 0 <= seg < 0x10000:
                    out.add((seg, int(o, 16)))
    return out


def lift_image(name, load_seg):
    src = os.path.join(DATA, IMAGES[name])
    im = Image(name, src)
    extra = load_misses(name, load_seg)
    # Walk, add the stored pointers the walk makes visible, walk again.
    fps = set()
    while True:
        entries, bodies, tables, bad = discover(im, extra | fps)
        more = stored_pointers(im, entries, bodies) - fps
        if not more:
            break
        fps |= more
    codesegs = sorted(set(entries.values()))
    print(f'{name} ({IMAGES[name]}) at {load_seg:04X}: {len(codesegs)} code segments, '
          f'{len(entries)} functions, {sum(len(b) for b in bodies.values())} instructions, '
          f'{len(extra)} recorded entries, {sum(len(t) for t in tables.values())} switch tables')
    if bad:
        print(f'  {len(bad)} undecodable targets, first: '
              + ', '.join(f'{c:04X}:{i:04X} (in {name}_{l:05X})' for c, i, l in bad[:5]))

    names = {lin: f'{name}_{lin:05X}' for lin in entries}
    lifter = Lifter(known_funcs=names, reloc_words=im.reloc_at, load_seg=load_seg)
    lifter.far_base = 0
    lifter.dispatch = True
    lifter.iret_frame = True
    lifter.x87 = False                   # the game has no floating point
    lifter.smc_imm = smc_targets(entries, bodies)
    print(f'  {len(lifter.smc_imm)} self-modified code bytes')
    lift16.DIV0_FN = 'recomp_div0'

    final = os.path.join(WORK, 'gen', name)
    out = final + '.new'
    os.makedirs(final, exist_ok=True)
    shutil.rmtree(out, ignore_errors=True)
    os.makedirs(out)

    order = sorted(entries)
    unhandled, files = {}, []
    chunks, cur, size = [], [], 0
    for lin in order:
        cur.append(lin)
        size += len(bodies[lin])
        if size >= CHUNK:
            chunks.append(cur)
            cur, size = [], 0
    if cur:
        chunks.append(cur)
    for ci, chunk in enumerate(chunks):
        path = os.path.join(out, f'{name}_{ci:03d}.c')
        files.append(path)
        with open(path, 'w') as f:
            f.write('/* generated by tools/lift.py from your own copy of the game -- do not commit */\n')
            f.write(f'#include "lifted.h"\n#include "{name}.h"\n\n')
            for lin in chunk:
                cs = entries[lin]
                insts = _seal_fallthrough([bodies[lin][a] for a in sorted(bodies[lin])])
                lift16._CODE_SEG = f'{cs:04X}'
                lifter.jump_tables = {ip: [cs * 16 + a for a in arms]
                                      for ip, arms in tables[lin].items()}
                body = lifter.lift_function(names[lin], insts, cs * 16, entry_addr=lin - cs * 16)
                for line in body.split('\n'):
                    if 'UNHANDLED' in line or 'needs dispatch' in line:
                        k = line.split('/*')[-1].strip(' */')
                        unhandled[k] = unhandled.get(k, 0) + 1
                f.write(body + '\n\n')

    # A far call lift16 could not resolve names a function that does not exist
    # in this image (another image's code, or bytes decoded past a call that
    # never returns). Each becomes a dispatch: the runtime finds the owner.
    strays, near = set(), set()
    for path in files:
        text = open(path).read()
        strays |= set(re.findall(r'\b(far_[0-9A-F]{4}_[0-9A-F]{4})\(cpu\)', text))
        near |= set(re.findall(r'\b(res_[0-9A-F]{6})\(cpu\)', text))
    with open(files[-1], 'a') as f:
        for n in sorted(strays):
            seg = (int(n[4:8], 16) + (load_seg if im.relocs else 0)) & 0xFFFF
            f.write(f'void {name}_{n}(CPU *cpu) {{ cpu->sp += 4; '
                    f'dispatch_far(cpu, 0x{seg:04X}, 0x{n[9:13]}); }}\n')
        # Near calls to an address nothing lifted (a call decoded from data,
        # or past the end of the image): dispatched the same way.
        for n in sorted(near):
            t = int(n[4:], 16)
            f.write(f'void {name}_{n}(CPU *cpu) {{ cpu->sp += 2; '
                    f'dispatch_near(cpu, 0x{(load_seg + (t >> 4)) & 0xFFFF:04X}, 0x{t & 0xF:X}); }}\n')
    if strays or near:
        print(f'  {len(strays)} far, {len(near)} near calls to no lifted function (dispatched)')

    with open(os.path.join(out, f'{name}.h'), 'w') as f:
        f.write('/* generated by tools/lift.py -- do not commit */\n#pragma once\n')
        for cs in codesegs:
            f.write(f'#define SEG_{cs:04X} 0x{(cs + load_seg) & 0xFFFF:04X}\n')
        for lin in order:
            f.write(f'void {names[lin]}(CPU *cpu);\n')
        for n in sorted(strays | near):       # per image: they are all linked together
            f.write(f'#define {n} {name}_{n}\nvoid {n}(CPU *cpu);\n')
    with open(os.path.join(out, f'{name}_table.c'), 'w') as f:
        f.write('/* generated by tools/lift.py -- do not commit */\n'
                f'#include "lifted.h"\n#include "{name}.h"\n')
        f.write(f'static const RecompFunc funcs[] = {{\n')
        for lin in order:
            f.write(f'  {{0x{lin:05X}, {names[lin]}}},\n')
        f.write('};\n')
        f.write(f'const LiftedImage g_img_{name} = {{"{name}", "{IMAGES[name]}", 0x{load_seg:04X}, '
                f'{len(im.raw)}u, 0x{_fnv(im.raw):08X}u, {len(order)}, funcs}};\n')
    json.dump({'image': name, 'load_seg': load_seg, 'functions': len(order),
               'codesegs': codesegs, 'unhandled': unhandled},
              open(os.path.join(out, 'lift.json'), 'w'), indent=1)
    if unhandled:
        print(f'  unhandled instruction forms: {sum(unhandled.values())} '
              f'({len(unhandled)} kinds), first: {list(unhandled)[:6]}')
    changed = sync(out, final)
    print(f'  -> {final} ({len(files)} files, {changed} changed)')


def _fnv(b):
    """FNV-1a 32: the runtime checks it to refuse a file this lift is not of."""
    h = 0x811C9DC5
    for x in b:
        h = ((h ^ x) * 0x01000193) & 0xFFFFFFFF
    return h


def main():
    want = sys.argv[1:] or list(IMAGES)
    layout = read_layout()
    missing = [n for n in IMAGES if not os.path.exists(os.path.join(DATA, IMAGES[n]))]
    if missing == list(IMAGES):
        sys.exit(f'no game files in {DATA} -- copy your own Epic Pinball install there '
                 '(see README, Getting Started)')
    done = []
    for name in want:
        if name not in IMAGES:
            sys.exit(f'unknown image {name}; one of: {" ".join(IMAGES)}')
        if name in missing:
            print(f'{name}: {IMAGES[name]} not in original/, skipped')
            continue
        if name not in layout:
            print(f'{name}: not placed yet (run the game once; src/images.c records it in work/layout.txt)')
            continue
        lift_image(name, layout[name])
        done.append(name)
    # The list of lifted images, for src/images.c. A source file of its own
    # rather than a header: MSBuild's include tracking missed a regenerated
    # header, and a newly lifted image was linked but never registered.
    have = sorted(n for n in IMAGES if os.path.exists(os.path.join(WORK, 'gen', n, f'{n}_table.c')))
    p = os.path.join(WORK, 'gen', 'images_list.c')
    with open(p + '.new', 'w') as f:
        f.write('/* generated by tools/lift.py -- do not commit */\n#include "lifted.h"\n')
        f.writelines(f'extern const LiftedImage g_img_{n};\n' for n in have)
        f.write('const LiftedImage *const g_lifted[] = {'
                + ''.join(f'&g_img_{n}, ' for n in have) + 'NULL};\n')
    if not (os.path.exists(p) and filecmp.cmp(p + '.new', p, shallow=False)):
        shutil.copyfile(p + '.new', p)
    os.remove(p + '.new')


def sync(src, dst):
    """Move src's files over dst's, touching only those whose contents differ,
    and drop dst's files src does not have: an unchanged lift leaves every
    timestamp alone, so the next build compiles nothing."""
    changed = 0
    for f in os.listdir(dst):
        if not os.path.exists(os.path.join(src, f)):
            os.remove(os.path.join(dst, f))
            changed += 1
    for f in os.listdir(src):
        a, b = os.path.join(src, f), os.path.join(dst, f)
        if os.path.exists(b) and filecmp.cmp(a, b, shallow=False):
            continue
        shutil.copyfile(a, b)
        changed += 1
    shutil.rmtree(src)
    return changed


def _seal_fallthrough(insts):
    """lift16 emits instructions in list order and lets C fall from one to the
    next. If a fallthrough successor is not the next one in the list -- two
    paths decoded the same bytes at different alignments -- make the edge an
    explicit jmp so C cannot fall into the wrong instruction."""
    out = []
    for i, ins in enumerate(insts):
        out.append(ins)
        m = ins.mnemonic
        if m in TERMINATORS or m in ('jmp', 'jmp far'):
            continue
        nxt = (ins.address + ins.length) & 0xFFFF
        if i + 1 < len(insts) and insts[i + 1].address == nxt:
            continue
        j = decode16.Instruction()
        j.mnemonic, j.address, j.offset, j.length = 'jmp', -1, ins.offset, 0
        j.raw = b''
        j.op1 = decode16.Operand(type=OpType.REL16, disp=nxt, size=2)
        out.append(j)
    return out


if __name__ == '__main__':
    main()
