#!/usr/bin/env python3
"""
sysmap.py - navigate AVer330USB.sys (checked build) by its own debug strings.

The driver is a checked build, so nearly every function DbgPrint()s its own name.
Combining that with .pdata (exact function bounds on x86-64 PE) gives a named
function map of the whole driver.

  list  PATTERN        functions whose referenced strings match PATTERN
  show  NAME|0xADDR    disassemble a function, annotated with strings + immediates
  xref  PATTERN        where a string is referenced from
  imm   NAME|0xADDR    just the distinct immediate constants in a function

Needs capstone.  Pure-python PE parsing, no external tools.
"""
import argparse, re, struct, sys, os
from capstone import *
from capstone.x86 import *

HERE = os.path.dirname(os.path.abspath(__file__))
DEFAULT_SYS = os.path.normpath(os.path.join(HERE, "..", "vendor", "AVer330USB.sys"))


class PE:
    def __init__(self, path):
        self.buf = open(path, "rb").read()
        b = self.buf
        e_lfanew = struct.unpack_from("<I", b, 0x3C)[0]
        assert b[e_lfanew:e_lfanew + 4] == b"PE\0\0", "not a PE"
        coff = e_lfanew + 4
        (self.machine, self.nsec, _, _, _, opt_size, _) = struct.unpack_from("<HHIIIHH", b, coff)
        opt = coff + 20
        magic = struct.unpack_from("<H", b, opt)[0]
        assert magic == 0x20B, f"expected PE32+, got {magic:#x}"
        self.image_base = struct.unpack_from("<Q", b, opt + 24)[0]
        sec = opt + opt_size
        self.sections = []
        for i in range(self.nsec):
            off = sec + i * 40
            name = b[off:off + 8].rstrip(b"\0").decode("latin1")
            vsize, vaddr, rawsize, rawptr = struct.unpack_from("<IIII", b, off + 8)
            self.sections.append(dict(name=name, vaddr=vaddr, vsize=vsize,
                                      rawsize=rawsize, rawptr=rawptr))

    def sec_of(self, rva):
        for s in self.sections:
            if s["vaddr"] <= rva < s["vaddr"] + max(s["vsize"], s["rawsize"]):
                return s
        return None

    def off(self, rva):
        s = self.sec_of(rva)
        if not s or rva - s["vaddr"] >= s["rawsize"]:
            return None
        return s["rawptr"] + (rva - s["vaddr"])

    def read(self, rva, n):
        o = self.off(rva)
        return None if o is None else self.buf[o:o + n]

    def section(self, name):
        for s in self.sections:
            if s["name"] == name:
                return s
        return None

    def cstr(self, rva, maxlen=300):
        o = self.off(rva)
        if o is None:
            return None
        end = self.buf.find(b"\0", o, o + maxlen)
        if end < 0:
            return None
        raw = self.buf[o:end]
        if len(raw) < 4:
            return None
        try:
            s = raw.decode("latin1")
        except Exception:
            return None
        printable = sum(1 for c in s if 0x20 <= ord(c) < 0x7F or c in "\t\r\n")
        return s if printable >= len(s) * 0.95 else None

    def functions(self):
        """RUNTIME_FUNCTION[] from .pdata: (begin_rva, end_rva, unwind_rva)."""
        pd = self.section(".pdata")
        out = []
        if not pd:
            return out
        data = self.buf[pd["rawptr"]:pd["rawptr"] + pd["rawsize"]]
        for i in range(0, len(data) - 11, 12):
            b, e, u = struct.unpack_from("<III", data, i)
            if b and e > b:
                out.append((b, e, u))
        out.sort()
        return out


def disasm(pe, start, end):
    md = Cs(CS_ARCH_X86, CS_MODE_64)
    md.detail = True
    code = pe.read(start, end - start)
    return list(md.disasm(code, start)) if code else []


def func_strings(pe, insns):
    """RIP-relative lea/mov targets in this function that point at C strings."""
    found = []
    for ins in insns:
        for op in ins.operands:
            if op.type == X86_OP_MEM and op.mem.base == X86_REG_RIP:
                tgt = ins.address + ins.size + op.mem.disp
                s = pe.cstr(tgt)
                if s:
                    found.append((ins.address, tgt, s))
    return found


def build_index(pe):
    """[(begin, end, [strings...])] for every function in .pdata."""
    idx = []
    for (b, e, _u) in pe.functions():
        ins = disasm(pe, b, e)
        if not ins:
            continue
        strs = [s for (_a, _t, s) in func_strings(pe, ins)]
        idx.append((b, e, strs))
    return idx


def name_of(strs):
    """Guess a function's name from its own debug strings, e.g. 'CFoo_Bar() ...'."""
    for s in strs:
        m = re.search(r"\b([A-Za-z_][A-Za-z0-9_]*(?:::|_)[A-Za-z0-9_]+)\s*\(\)", s)
        if m:
            return m.group(1)
    for s in strs:
        m = re.search(r"\b([A-Z][A-Za-z0-9_]{3,})\(", s)
        if m:
            return m.group(1)
    return None


def resolve(pe, idx, who):
    if who.startswith("0x"):
        a = int(who, 16)
        for (b, e, strs) in idx:
            if b <= a < e:
                return (b, e, strs)
        return None
    pat = re.compile(re.escape(who), re.I)
    hits = [(b, e, s) for (b, e, s) in idx if any(pat.search(x) for x in s)]
    if not hits:
        return None
    # prefer the function whose derived name matches most tightly
    hits.sort(key=lambda t: (0 if (name_of(t[2]) or "").lower() == who.lower() else 1,
                             len(t[2])))
    return hits[0]


def cmd_list(pe, idx, args):
    pat = re.compile(args.pattern, re.I)
    rows = []
    for (b, e, strs) in idx:
        if any(pat.search(s) for s in strs):
            rows.append((b, e, name_of(strs) or "?", len(strs)))
    rows.sort()
    print(f"{len(rows)} function(s) referencing /{args.pattern}/\n")
    for (b, e, nm, n) in rows:
        print(f"  0x{b:06x}-0x{e:06x}  {e-b:>6} B  {n:>3} str  {nm}")


def cmd_xref(pe, idx, args):
    pat = re.compile(args.pattern, re.I)
    for (b, e, strs) in idx:
        ins = disasm(pe, b, e)
        for (a, t, s) in func_strings(pe, ins):
            if pat.search(s):
                print(f"  0x{a:06x}  in {name_of(strs) or hex(b):<36} -> {s[:110]!r}")


INTERESTING = re.compile(r"^(mov|cmp|add|sub|or|and|test|push|lea)")


def cmd_show(pe, idx, args):
    f = resolve(pe, idx, args.who)
    if not f:
        print(f"no function matching {args.who!r}")
        return
    b, e, strs = f
    print(f"=== {name_of(strs) or '?'}   0x{b:06x}-0x{e:06x}  ({e-b} bytes) ===")
    if strs:
        print("--- strings referenced ---")
        for s in dict.fromkeys(strs):
            print(f"    {s[:150]!r}")
    print("--- disassembly ---")
    ins = disasm(pe, b, e)
    srefs = {a: s for (a, _t, s) in func_strings(pe, ins)}
    for i in ins:
        line = f"  {i.address:06x}:  {i.mnemonic:<7} {i.op_str}"
        if i.address in srefs:
            line += f"      ; {srefs[i.address][:70]!r}"
        elif i.mnemonic == "call":
            tgt = i.op_str
            if tgt.startswith("0x"):
                t = int(tgt, 16)
                g = resolve(pe, idx, hex(t))
                if g and g[0] == t:
                    nm = name_of(g[2])
                    if nm:
                        line += f"      ; -> {nm}"
        print(line)


def cmd_imm(pe, idx, args):
    f = resolve(pe, idx, args.who)
    if not f:
        print(f"no function matching {args.who!r}")
        return
    b, e, strs = f
    print(f"=== {name_of(strs) or '?'}  0x{b:06x}-0x{e:06x} : immediates ===")
    seen = {}
    for i in disasm(pe, b, e):
        for op in i.operands:
            if op.type == X86_OP_IMM:
                v = op.imm & 0xFFFFFFFFFFFFFFFF
                if v > 1:
                    seen.setdefault(v, []).append(i.address)
    for v in sorted(seen):
        where = " ".join(f"{a:x}" for a in seen[v][:6])
        print(f"  0x{v:<10x} {v:>12}   @ {where}")


VT = {0xb0: "HciRegWrite?", 0x1a8: "RegRead?", 0x1b0: "RegisterWrite",
      0x1b8: "vt+0x1b8"}

_IMM = re.compile(r"^(e?d[xi]|dx|r8d|r9d|r8w|ecx|cx),\s*(0x[0-9a-f]+|\d+)$", re.I)
_CALLVT = re.compile(r"^qword ptr \[rax \+ (0x[0-9a-f]+)\]$")


def cmd_writes(pe, idx, args):
    """Reconstruct the (register, value) sequence a bring-up function writes.

    These functions all follow one shape: load the register number into dx and the
    value into r8d, then make an indirect vtable call. Track the last immediate
    seen in each and emit a line per call.
    """
    f = resolve(pe, idx, args.who)
    if not f:
        print(f"no function matching {args.who!r}")
        return
    b, e, strs = f
    print(f"=== {name_of(strs) or '?'}  0x{b:06x}-0x{e:06x} : write sequence ===")
    print(f"{'#':>3}  {'at':>8}  {'reg':>7}  {'value':>12}  via")
    held = {}
    n = 0
    for i in disasm(pe, b, e):
        m = _IMM.match(i.op_str) if i.mnemonic == "mov" else None
        if m:
            held[m.group(1).lower()] = int(m.group(2), 0)
            continue
        # any other write to a tracked reg invalidates the immediate we held
        if i.mnemonic in ("mov", "movzx", "movsxd", "lea", "imul", "add", "or",
                          "xor", "and", "sub", "shl", "shr"):
            dst = i.op_str.split(",")[0].strip().lower()
            if dst in held:
                held[dst] = None
            if dst in ("edx", "rdx") and "dx" in held:
                held["dx"] = None
            if dst in ("r8", "r8d") and "r8d" in held:
                held["r8d"] = None
            continue
        if i.mnemonic == "call":
            cm = _CALLVT.match(i.op_str)
            if not cm:
                continue
            off = int(cm.group(1), 0)
            reg = held.get("dx")
            val = held.get("r8d")
            n += 1
            rs = f"0x{reg:04x}" if reg is not None else "(computed)"
            vs = f"0x{val:08x}" if val is not None else "(from memory)"
            print(f"{n:>3}  {i.address:08x}  {rs:>7}  {vs:>12}  {VT.get(off, hex(off))}")
    if not n:
        print("  (no vtable write calls found)")


_LEA_RIP = re.compile(r"^(r[a-z0-9]+),\s*\[rip \+ (0x[0-9a-f]+)\]$")
_MOV_FIELD = re.compile(r"^qword ptr \[(r[a-z0-9]+) \+ (0x[0-9a-f]+)\],\s*(r[a-z0-9]+)$")


def cmd_vtable(pe, idx, args):
    """Resolve object function-pointer slots.

    These objects don't use a C++ vtable; the constructor stores function pointers
    into fields of the object itself (`mov [rcx+0x1b0], <CUsbCntl_RegisterWrite>`).
    Pair each `lea reg,[rip+X]` that lands on a known function with the following
    store, and we get slot -> function name for every indirect call site.
    """
    names = {}
    for (b, e, strs) in idx:
        n = name_of(strs)
        if n:
            names[b] = n

    f = resolve(pe, idx, args.who)
    if not f:
        print(f"no function matching {args.who!r}")
        return
    b, e, strs = f
    print(f"=== {name_of(strs) or '?'}  0x{b:06x}-0x{e:06x} : function-pointer slots ===")
    text = pe.section(".text")
    lo, hi = text["vaddr"], text["vaddr"] + text["rawsize"]

    held = {}      # reg -> (target_rva, name)
    found = {}
    for i in disasm(pe, b, e):
        if i.mnemonic == "lea":
            m = _LEA_RIP.match(i.op_str)
            if m:
                tgt = i.address + i.size + int(m.group(2), 0)
                if lo <= tgt < hi:
                    held[m.group(1)] = (tgt, names.get(tgt))
                else:
                    held.pop(m.group(1), None)
            continue
        if i.mnemonic == "mov":
            m = _MOV_FIELD.match(i.op_str)
            if m and m.group(3) in held:
                off = int(m.group(2), 0)
                tgt, nm = held[m.group(3)]
                found[off] = (tgt, nm)
    if not found:
        print("  (no function-pointer stores found here)")
        return
    for off in sorted(found):
        tgt, nm = found[off]
        print(f"  +0x{off:04x}  ->  0x{tgt:06x}  {nm or '(unnamed)'}")


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--sys", default=DEFAULT_SYS)
    sub = ap.add_subparsers(dest="cmd", required=True)
    s = sub.add_parser("list"); s.add_argument("pattern"); s.set_defaults(fn=cmd_list)
    s = sub.add_parser("show"); s.add_argument("who"); s.set_defaults(fn=cmd_show)
    s = sub.add_parser("xref"); s.add_argument("pattern"); s.set_defaults(fn=cmd_xref)
    s = sub.add_parser("imm");  s.add_argument("who"); s.set_defaults(fn=cmd_imm)
    s = sub.add_parser("writes"); s.add_argument("who"); s.set_defaults(fn=cmd_writes)
    s = sub.add_parser("vtable"); s.add_argument("who"); s.set_defaults(fn=cmd_vtable)
    args = ap.parse_args()

    pe = PE(args.sys)
    idx = build_index(pe)
    args.fn(pe, idx, args)


if __name__ == "__main__":
    main()
