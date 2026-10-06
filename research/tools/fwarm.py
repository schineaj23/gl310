#!/usr/bin/env python3
"""
fwarm.py - first-pass analysis of qpvidfwusb.bin (the GL310's ARM32 firmware).

The image is a flat ARM32 little-endian binary with its exception vector table at
offset 0, loaded at base 0x00000000 (see ../notes/RE.md).

  vectors        decode the 8-entry exception vector table + its literal pool
  strings        printable strings, with offsets (version / build / subsystem names)
  entry          disassemble from the reset vector's target
  pools          32-bit literal-pool constants that look like MMIO addresses
  at 0xADDR      disassemble N instructions at an address

Needs capstone.
"""
import argparse, collections, os, re, struct, sys
from capstone import *
from capstone.arm import *

HERE = os.path.dirname(os.path.abspath(__file__))
DEFAULT_FW = os.path.normpath(os.path.join(HERE, "..", "vendor", "qpvidfwusb.bin"))

VEC_NAMES = ["reset", "undefined", "SWI", "prefetch_abort",
             "data_abort", "reserved", "IRQ", "FIQ"]


def md():
    m = Cs(CS_ARCH_ARM, CS_MODE_ARM)
    m.detail = True
    return m


def u32(b, off):
    return struct.unpack_from("<I", b, off)[0]


def cmd_vectors(fw, args):
    print("Exception vector table @ 0x00000000\n")
    m = md()
    for i in range(8):
        off = i * 4
        word = u32(fw, off)
        ins = list(m.disasm(fw[off:off + 4], off))
        txt = f"{ins[0].mnemonic} {ins[0].op_str}" if ins else "(undecodable)"
        line = f"  0x{off:02x}  {word:08x}  {VEC_NAMES[i]:<15} {txt}"
        # LDR PC, [PC, #imm] -> target address is in a literal pool slot
        mm = re.match(r"ldr\s+pc,\s*\[pc,\s*#(\d+|0x[0-9a-f]+)\]", txt.strip(), re.I)
        if mm:
            imm = int(mm.group(1), 0)
            slot = off + 8 + imm           # ARM pipeline: PC reads as insn+8
            if slot + 4 <= len(fw):
                line += f"   -> [0x{slot:04x}] = 0x{u32(fw, slot):08x}"
        print(line)

    print("\nLiteral pool after the table:")
    for off in range(0x20, 0x60, 4):
        print(f"  [0x{off:04x}] = 0x{u32(fw, off):08x}")


def iter_strings(fw, minlen=6):
    cur, start = [], 0
    for i, c in enumerate(fw):
        if 0x20 <= c < 0x7F:
            if not cur:
                start = i
            cur.append(chr(c))
        else:
            if len(cur) >= minlen:
                yield start, "".join(cur)
            cur = []
    if len(cur) >= minlen:
        yield start, "".join(cur)


def cmd_strings(fw, args):
    pat = re.compile(args.grep, re.I) if args.grep else None
    n = 0
    for off, s in iter_strings(fw, args.min):
        if pat and not pat.search(s):
            continue
        print(f"  0x{off:06x}  {s}")
        n += 1
        if args.max and n >= args.max:
            print(f"  ... (stopped at {args.max})")
            break
    if not n:
        print("  (none)")


def cmd_entry(fw, args):
    reset_word = u32(fw, 0)
    m = md()
    ins = list(m.disasm(fw[0:4], 0))
    target = None
    if ins:
        mm = re.match(r"\[pc, #(\d+|0x[0-9a-f]+)\]", ins[0].op_str.split(",", 1)[-1].strip(), re.I)
        mm = re.search(r"#(\d+|0x[0-9a-f]+)", ins[0].op_str)
        if mm:
            slot = 0 + 8 + int(mm.group(1), 0)
            target = u32(fw, slot)
    if target is None:
        print("could not resolve reset target")
        return
    print(f"reset vector -> 0x{target:08x}\n")
    if target >= len(fw):
        print(f"  (outside the {len(fw)}-byte image: it runs from somewhere else)")
        return
    for i in m.disasm(fw[target:target + args.n * 4], target):
        print(f"  {i.address:06x}:  {i.mnemonic:<8} {i.op_str}")


def cmd_at(fw, args):
    a = int(args.addr, 16)
    m = md()
    for i in m.disasm(fw[a:a + args.n * 4], a):
        print(f"  {i.address:06x}:  {i.mnemonic:<8} {i.op_str}")


PCREL = re.compile(r"\[pc,? ?#(-?)(?:0x)?([0-9a-f]+)\]", re.I)


def cmd_pools(fw, args):
    """Collect PC-relative literal loads and bucket the constants they pull in.

    ARM32 is fixed-width, so a literal pool (data) stops linear disassembly.
    Restart at the next aligned word whenever capstone gives up.
    """
    m = Cs(CS_ARCH_ARM, CS_MODE_ARM)
    consts = collections.Counter()
    pos, n_insn = 0, 0
    end = len(fw) & ~3
    while pos < end:
        last = pos
        for (addr, size, mnem, ops) in m.disasm_lite(fw[pos:end], pos):
            last = addr + size
            n_insn += 1
            if not mnem.startswith("ldr"):
                continue
            mm = PCREL.search(ops)
            if not mm:
                continue
            disp = int(mm.group(2), 16) * (-1 if mm.group(1) else 1)
            slot = (addr + 8 + disp) & ~3
            if 0 <= slot and slot + 4 <= len(fw):
                consts[u32(fw, slot)] += 1
        pos = max(last, pos + 4)
    print(f"decoded {n_insn:,} instructions")
    print(f"{len(consts)} distinct PC-relative literal constants\n")
    print("Grouped by 0x100000 region (candidate MMIO windows):")
    regions = collections.Counter()
    for v, n in consts.items():
        regions[v & 0xFFF00000] += n
    for base, n in regions.most_common(14):
        print(f"  0x{base:08x}xxxxx  {n:>6} refs")
    lo, hi = args.lo, args.hi
    print(f"\nConstants in [0x{lo:x}, 0x{hi:x}] (control-page candidates), by ref count:")
    rows = [(v, n) for v, n in consts.items() if lo <= v <= hi]
    rows.sort(key=lambda t: -t[1])
    for v, n in rows[:40]:
        print(f"  0x{v:08x}   {n:>5} refs")
    if not rows:
        print("  (none)")


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--fw", default=DEFAULT_FW)
    sub = ap.add_subparsers(dest="cmd", required=True)
    s = sub.add_parser("vectors"); s.set_defaults(fn=cmd_vectors)
    s = sub.add_parser("strings"); s.add_argument("--grep"); s.add_argument("--min", type=int, default=6)
    s.add_argument("--max", type=int, default=60); s.set_defaults(fn=cmd_strings)
    s = sub.add_parser("entry"); s.add_argument("-n", type=int, default=40); s.set_defaults(fn=cmd_entry)
    s = sub.add_parser("at"); s.add_argument("addr"); s.add_argument("-n", type=int, default=40)
    s.set_defaults(fn=cmd_at)
    s = sub.add_parser("pools"); s.add_argument("--lo", type=lambda x: int(x,0), default=0x400)
    s.add_argument("--hi", type=lambda x: int(x,0), default=0x1000); s.set_defaults(fn=cmd_pools)
    args = ap.parse_args()
    fw = open(args.fw, "rb").read()
    args.fn(fw, args)


if __name__ == "__main__":
    main()
