#!/usr/bin/env python3
"""
gl310cap.py - decode USBPcap captures of the AVerMedia GL310 (07ca:c835).

Pure standard library; runs anywhere with Python 3.8+ (Windows, macOS, Linux).

  summary  CAP              what is in the capture, per endpoint, plus verdicts
  fw       CAP              locate firmware bytes in OUT transfers and infer the
                            framing header (offset / length / opcode fields)
  dump     CAP [--ep EP]    one line per transfer, hex-prefixed, for diffing
  extract  CAP --ep EP -o F concatenate an endpoint's payloads into a file
  diff     CAP_A CAP_B      opcode-ish header shapes present in one but not the other

The device is auto-detected from its device descriptor (or from traffic on
its four bulk endpoints). Override with --bus/--addr if detection fails.
"""
import argparse
import collections
import glob
import os
import struct
import sys

VID, PID = 0x07CA, 0xC835
PIDS = {0xC835, 0xD835}
BULK_EPS = {0x02, 0x04, 0x81, 0x83}
LINKTYPE_USBPCAP = 249
FW_MAGIC = {
    "qpvidfwusb.bin (ARM)": bytes.fromhex("18f09fe5"),
    "qpaudfwusb.bin (DSP)": bytes.fromhex("2020800f"),
}
XFER = {0: "ISO", 1: "INT", 2: "CTRL", 3: "BULK", 0xFE: "IRP", 0xFF: "?"}
HERE = os.path.dirname(os.path.abspath(__file__))
VENDOR = os.path.normpath(os.path.join(HERE, "..", "vendor"))


# --------------------------------------------------------------------------
# capture file readers (pcapng + legacy pcap)
# --------------------------------------------------------------------------

def _read_pcapng(f):
    ifaces = []  # (linktype, ts_divisor)
    endian = "<"
    while True:
        hdr = f.read(8)
        if len(hdr) < 8:
            return
        btype_raw, = struct.unpack("<I", hdr[:4])
        if btype_raw == 0x0A0D0D0A:  # SHB: work out byte order first
            bom = f.read(4)
            endian = "<" if bom == b"\x4d\x3c\x2b\x1a" else ">"
            blen, = struct.unpack(endian + "I", hdr[4:8])
            f.read(blen - 12)
            ifaces = []
            continue
        btype, blen = struct.unpack(endian + "II", hdr)
        body = f.read(blen - 8)
        if len(body) < blen - 8:
            return
        body = body[:-4]  # trailing length
        if btype == 1:  # IDB
            linktype, _, _snap = struct.unpack(endian + "HHI", body[:8])
            div = 1_000_000
            opts = body[8:]
            i = 0
            while i + 4 <= len(opts):
                code, olen = struct.unpack(endian + "HH", opts[i:i + 4])
                if code == 0:
                    break
                val = opts[i + 4:i + 4 + olen]
                if code == 9 and olen >= 1:  # if_tsresol
                    r = val[0]
                    div = 2 ** (r & 0x7F) if r & 0x80 else 10 ** r
                i += 4 + ((olen + 3) & ~3)
            ifaces.append((linktype, div))
        elif btype == 6:  # EPB
            iid, tsh, tsl, caplen, origlen = struct.unpack(endian + "IIIII", body[:20])
            lt, div = ifaces[iid] if iid < len(ifaces) else (None, 1_000_000)
            ts = ((tsh << 32) | tsl) / div
            yield lt, ts, body[20:20 + caplen], origlen
        elif btype == 3:  # SPB
            origlen, = struct.unpack(endian + "I", body[:4])
            lt, div = ifaces[0] if ifaces else (None, 1_000_000)
            yield lt, 0.0, body[4:4 + origlen], origlen


def _read_pcap(f, magic):
    table = {
        b"\xd4\xc3\xb2\xa1": ("<", 1e6), b"\xa1\xb2\xc3\xd4": (">", 1e6),
        b"\x4d\x3c\xb2\xa1": ("<", 1e9), b"\xa1\xb2\x3c\x4d": (">", 1e9),
    }
    e, div = table[magic]
    rest = f.read(20)
    linktype = struct.unpack(e + "I", rest[16:20])[0] & 0x0FFFFFFF
    while True:
        h = f.read(16)
        if len(h) < 16:
            return
        s, frac, caplen, origlen = struct.unpack(e + "IIII", h)
        yield linktype, s + frac / div, f.read(caplen), origlen


def read_capture(path):
    with open(path, "rb") as f:
        magic = f.read(4)
        f.seek(0)
        if magic == b"\x0a\x0d\x0d\x0a":
            yield from _read_pcapng(f)
        elif magic in (b"\xd4\xc3\xb2\xa1", b"\xa1\xb2\xc3\xd4",
                       b"\x4d\x3c\xb2\xa1", b"\xa1\xb2\x3c\x4d"):
            f.read(4)
            yield from _read_pcap(f, magic)
        else:
            sys.exit(f"{path}: not a pcap/pcapng file")


# --------------------------------------------------------------------------
# USBPcap pseudo-header
# --------------------------------------------------------------------------

class Pkt:
    __slots__ = ("idx", "ts", "irp", "status", "func", "completion", "bus",
                 "addr", "ep", "xfer", "stage", "data", "datalen", "truncated")

    @property
    def is_in(self):
        return bool(self.ep & 0x80)

    @property
    def carries_payload(self):
        # OUT data rides on the submission (FDO->PDO); IN data on the completion.
        return self.completion == self.is_in


def parse_usbpcap(cap):
    for idx, (lt, ts, raw, origlen) in enumerate(read_capture(cap), 1):
        if lt != LINKTYPE_USBPCAP or len(raw) < 27:
            continue
        (hlen, irp, status, func, info, bus, addr, ep, xfer,
         dlen) = struct.unpack("<HQIHBHHBBI", raw[:27])
        p = Pkt()
        p.idx, p.ts, p.irp, p.status, p.func = idx, ts, irp, status, func
        p.completion = bool(info & 1)
        p.bus, p.addr, p.ep, p.xfer = bus, addr, ep, xfer
        p.stage = raw[27] if xfer == 2 and hlen >= 28 else None
        p.data = raw[hlen:]
        p.datalen = dlen
        p.truncated = len(p.data) < dlen or len(raw) < origlen
        yield p


def find_device(pkts, bus=None, addr=None):
    """Return (bus, addr) of the GL310."""
    if bus is not None and addr is not None:
        return bus, addr
    for p in pkts:  # device descriptor in a control completion
        d = p.data
        if p.xfer == 2 and len(d) >= 12 and d[0] == 18 and d[1] == 1:
            vid, pid = struct.unpack("<HH", d[8:12])
            if vid == VID and pid in PIDS:
                return p.bus, p.addr
    score = collections.Counter()
    for p in pkts:  # fall back: whoever uses all four bulk endpoints
        if p.xfer == 3 and p.ep in BULK_EPS:
            score[(p.bus, p.addr, p.ep)] += 1
    per_dev = collections.Counter()
    for (b, a, _ep) in score:
        per_dev[(b, a)] += 1
    best = [k for k, v in per_dev.items() if v == 4]
    if len(best) == 1:
        return best[0]
    return None


def load(cap, args):
    pkts = list(parse_usbpcap(cap))
    if not pkts:
        sys.exit(f"{cap}: no USBPcap (linktype 249) packets")
    dev = find_device(pkts, getattr(args, "bus", None), getattr(args, "addr", None))
    if dev is None:
        sys.exit(f"{cap}: GL310 not found. Was the capture on the right root hub? "
                 "Try --bus/--addr (see `usb.device_address` in Wireshark).")
    mine = [p for p in pkts if (p.bus, p.addr) == dev]
    return pkts, mine, dev


def payloads(mine, ep=None):
    """Data-bearing bulk events (OUT submissions, IN completions)."""
    for p in mine:
        if p.xfer == 3 and p.carries_payload and p.data and (ep is None or p.ep == ep):
            yield p


def hexs(b, n=None):
    b = b if n is None else b[:n]
    return " ".join(b[i:i + 4].hex() for i in range(0, len(b), 4))


# --------------------------------------------------------------------------
# summary
# --------------------------------------------------------------------------

def cmd_summary(args):
    pkts, mine, dev = load(args.cap, args)
    t0 = mine[0].ts
    print(f"capture     {args.cap}")
    print(f"packets     {len(pkts)} total, {len(mine)} for GL310 at bus {dev[0]} addr {dev[1]}")
    print(f"span        {mine[-1].ts - t0:.3f} s")
    trunc = sum(1 for p in mine if p.truncated)
    print(f"truncated   {trunc}" + ("   <-- SNAPLEN TOO SHORT, recapture" if trunc else "   (good)"))

    errs = [p for p in mine if p.completion and p.status not in (0,)]
    print(f"errors      {len(errs)} completions with nonzero USBD status")
    for p in errs[:8]:
        print(f"            #{p.idx} ep 0x{p.ep:02x} {XFER.get(p.xfer)} status 0x{p.status:08x}")

    print("\nendpoint    dir   xfers      bytes   min    max  common sizes")
    by_ep = collections.defaultdict(list)
    for p in payloads(mine):
        by_ep[p.ep].append(p)
    for ep in sorted(by_ep):
        ps = by_ep[ep]
        sizes = [len(p.data) for p in ps]
        common = ", ".join(f"{s}x{c}" for s, c in collections.Counter(sizes).most_common(4))
        print(f"0x{ep:02x}        {'IN ' if ep & 0x80 else 'OUT'}  {len(ps):6d} {sum(sizes):10d} "
              f"{min(sizes):5d} {max(sizes):6d}  {common}")

    ctrl = [p for p in mine if p.xfer == 2 and p.stage == 0 and len(p.data) >= 8]
    vendor = [p for p in ctrl if (p.data[0] >> 5) & 3 == 2]
    print(f"\ncontrol     {len(ctrl)} setups, {len(vendor)} vendor-type (bmRequestType bits 6:5 = 2)")
    for (req, rt), n in collections.Counter((p.data[1], p.data[0]) for p in vendor).most_common(8):
        print(f"            bRequest 0x{req:02x} bmRequestType 0x{rt:02x}  x{n}")

    print("\nfirmware landmarks (OUT payloads)")
    out_blob = {ep: b"".join(p.data for p in ps) for ep, ps in by_ep.items() if not ep & 0x80}
    for name, magic in FW_MAGIC.items():
        hits = [(ep, blob.find(magic)) for ep, blob in out_blob.items() if magic in blob]
        if hits:
            print(f"  {name:22s} FOUND  " + ", ".join(f"ep 0x{e:02x}" for e, _ in hits))
        else:
            print(f"  {name:22s} not found")

    print("\nbitstream hints (IN payloads)")
    for ep, ps in by_ep.items():
        if not ep & 0x80:
            continue
        blob = b"".join(p.data for p in ps)
        sc = blob.count(b"\x00\x00\x00\x01") + blob.count(b"\x00\x00\x01")
        nal = collections.Counter()
        i = blob.find(b"\x00\x00\x01")
        while i != -1 and i + 3 < len(blob):
            nal[blob[i + 3] & 0x1F] += 1
            i = blob.find(b"\x00\x00\x01", i + 3)
        ts_sync = _ts_sync_fraction(blob)
        names = {1: "slice", 5: "IDR", 6: "SEI", 7: "SPS", 8: "PPS", 9: "AUD"}
        top = ", ".join(f"{names.get(k, k)}x{v}" for k, v in nal.most_common(6))
        print(f"  0x{ep:02x}  {len(blob):10d} B  start codes {sc:6d}  "
              f"TS-sync {ts_sync:4.0%}  NAL: {top or '-'}")
    print("  (H.264 elementary stream: many start codes, SPS/PPS/IDR present."
          "\n   MPEG-TS: TS-sync near 100%. Status/mailbox pipe: few, small transfers.)")


def _ts_sync_fraction(blob):
    if len(blob) < 188 * 8:
        return 0.0
    best = 0.0
    for off in range(188):
        n = (len(blob) - off) // 188
        if n < 8:
            break
        hits = sum(1 for k in range(min(n, 400)) if blob[off + 188 * k] == 0x47)
        best = max(best, hits / min(n, 400))
    return best


# --------------------------------------------------------------------------
# firmware framing
# --------------------------------------------------------------------------

K = 16


class FwIndex:
    def __init__(self, path):
        self.name = os.path.basename(path)
        self.blob = open(path, "rb").read()
        self.idx = {}
        b = self.blob
        for i in range(len(b) - K + 1):
            self.idx.setdefault(b[i:i + K], i)

    def locate(self, data, max_hdr=512):
        """Return (hdr_len, fw_off, n) for the run of fw bytes starting at the
        smallest header length h, or None. Low-entropy windows (zero padding,
        fill patterns) are skipped so padding shared between images or with
        unrelated traffic does not produce false hits."""
        b = self.blob
        for h in range(0, min(max_hdr, len(data) - K) + 1):
            w = data[h:h + K]
            if len(set(w)) < 6:
                continue
            off = self.idx.get(w)
            if off is None:
                continue
            tail = data[h:]
            if b[off:off + len(tail)] != tail:
                j = b.find(tail)  # repeated window (e.g. vector table): search exactly
                if j != -1:
                    off = j
            n = 0
            while h + n < len(data) and off + n < len(b) and data[h + n] == b[off + n]:
                n += 1
            if n >= 256 or h + n == len(data) and n >= 64:
                if len(set(data[h:h + n])) >= 16:
                    while h > 0 and off > 0 and data[h - 1] == b[off - 1]:
                        h, off, n = h - 1, off - 1, n + 1  # chunk began in padding
                    return h, off, n
        return None


def cmd_fw(args):
    _, mine, dev = load(args.cap, args)
    fws = args.fw or sorted(glob.glob(os.path.join(VENDOR, "qp*fwusb.bin")))
    if not fws:
        sys.exit("no firmware images; pass --fw path/to/qpvidfwusb.bin")
    outs = [p for p in payloads(mine) if not p.is_in]
    images = [FwIndex(path) for path in fws]
    rows_by = {fw.name: [] for fw in images}
    prev = None  # most recent non-fw OUT transfer on any endpoint
    for p in outs:
        best = None
        for fw in images:  # a transfer belongs to whichever image explains most of it
            loc = fw.locate(p.data)
            if loc and (best is None or loc[2] > best[1][2]):
                best = (fw, loc)
        if best is None:
            prev = p
            continue
        h, off, n = best[1]
        rows_by[best[0].name].append((p, h, off, n, prev))
        prev = None
    for fw in images:
        rows = rows_by[fw.name]
        print(f"\n=== {fw.name}  ({len(fw.blob)} bytes)")
        if not rows:
            print("  not found in any OUT payload")
            continue
        # Backward extension into padding can eat the last header bytes when they
        # happen to equal the preceding fw byte; snap those rows to the modal length.
        H = collections.Counter(r[1] for r in rows).most_common(1)[0][0]
        rows = [(p, H, off + (H - h), n - (H - h), pre) if h < H else (p, h, off, n, pre)
                for p, h, off, n, pre in rows]
        covered = sum(r[3] for r in rows)
        first, last = rows[0][0], rows[-1][0]
        print(f"  (chunks lying wholly in zero padding cannot be attributed, so <100% is normal)")
        print(f"  {len(rows)} transfers on ep {sorted({'0x%02x' % r[0].ep for r in rows})}, "
              f"{covered} fw bytes ({covered / len(fw.blob):.1%} of image), "
              f"#{first.idx}..#{last.idx}, {last.ts - first.ts:.3f} s")
        hdr_lens = collections.Counter(r[1] for r in rows)
        print(f"  header length in-band: {dict(hdr_lens)}")
        print(f"  fw-offset order: {'monotonic' if all(rows[i][2] <= rows[i + 1][2] for i in range(len(rows) - 1)) else 'NOT monotonic'}")
        print("\n  first transfers (header | fw offset | fw bytes | preceding non-fw OUT command, any ep)")
        for p, h, off, n, pre in rows[:args.show]:
            pre_s = f"#{pre.idx} {len(pre.data)}B: {hexs(pre.data, 48)}" if pre else "-"
            print(f"  #{p.idx:<6} ep 0x{p.ep:02x} hdr[{h}] {hexs(p.data[:h]) or '(none)'}")
            print(f"           fw+0x{off:06x}  {n} B   tail-after-fw {len(p.data) - h - n} B   pre: {pre_s}")
        _infer_fields(rows, fw)
        _infer_fields_preceding(rows)


def _words(b, e="<"):
    return [struct.unpack_from(e + "I", b, i)[0] for i in range(0, len(b) - 3, 4)]


def _guess(rows_bytes, wi, offs, ns, tot):
    for e, tag in (("<", ""), (">", " [big-endian]")):
        vals = [_words(b, e)[wi] for b in rows_bytes]
        c = _classify(vals, offs, ns, tot)
        if c and not (e == ">" and c.startswith("constant")):
            return c + tag
    vals = [_words(b)[wi] for b in rows_bytes]
    return "varies: " + ", ".join("0x%x" % v for v in vals[:5])


def _classify(vals, offs, ns, tot):
    if len(set(vals)) == 1:
        common_n = collections.Counter(ns).most_common(1)[0][0]
        return f"constant 0x{vals[0]:x}" + ("  (= usual chunk length)" if vals[0] == common_n else "")
    d = {v - o for v, o in zip(vals, offs)}
    if len(d) == 1:
        base = d.pop()
        return f"FW ADDRESS (fw_offset + 0x{base & 0xffffffff:x})"
    if vals == ns:
        return "FW CHUNK LENGTH"
    if vals == tot:
        return "TOTAL TRANSFER LENGTH"
    if all(b - a == 1 for a, b in zip(vals, vals[1:])):
        return "SEQUENCE +1"
    if all(b > a for a, b in zip(vals, vals[1:])):
        return "INCREASING (counter with gaps?)"
    if sum(v == n for v, n in zip(vals, ns)) >= 0.9 * len(vals):
        return "FW CHUNK LENGTH (mostly)"
    if sum(v - o == vals[0] - offs[0] for v, o in zip(vals, offs)) >= 0.9 * len(vals):
        return f"FW ADDRESS (mostly; fw_offset + 0x{(vals[0] - offs[0]) & 0xffffffff:x})"
    return None


def _infer_fields(rows, fw):
    lens = collections.Counter(r[1] for r in rows if r[1] >= 4)
    if not lens:
        return
    h = lens.most_common(1)[0][0]  # modal header length; odd ones are padding artefacts
    with_hdr = [r for r in rows if r[1] == h]
    if len(with_hdr) < 3:
        return
    offs = [r[2] for r in with_hdr]
    ns = [r[3] for r in with_hdr]
    tot = [len(r[0].data) for r in with_hdr]
    print(f"\n  in-band header field guesses (u32 LE, over {len(with_hdr)} chunks):")
    hb = [r[0].data[:h] for r in with_hdr]
    for wi in range(h // 4):
        print(f"    +0x{wi * 4:02x}  {_guess(hb, wi, offs, ns, tot)}")


def _infer_fields_preceding(rows):
    pre = [r for r in rows if r[4] is not None and len(r[4].data) >= 8]
    if len(pre) < 3:
        return
    h = min(len(r[4].data) for r in pre)
    offs = [r[2] for r in pre]
    ns = [r[3] for r in pre]
    tot = [len(r[0].data) for r in pre]
    print(f"\n  out-of-band command guesses (preceding OUT packet, u32 LE, over {len(pre)} chunks):")
    hb = [r[4].data[:h] for r in pre]
    for wi in range(min(h, 64) // 4):
        print(f"    +0x{wi * 4:02x}  {_guess(hb, wi, offs, ns, tot)}")


# --------------------------------------------------------------------------
# dump / extract / diff
# --------------------------------------------------------------------------

def cmd_dump(args):
    _, mine, dev = load(args.cap, args)
    t0 = mine[0].ts
    ep = int(args.ep, 0) if args.ep else None
    n = 0
    for p in mine:
        if ep is not None and p.ep != ep:
            continue
        if p.xfer == 3 and not p.carries_payload and not args.all:
            continue
        if p.xfer == 2 and not args.all and p.stage not in (0, 3):
            continue
        tag = "CMPL" if p.completion else "SUBM"
        st = f" st={p.status:08x}" if p.completion and p.status else ""
        print(f"#{p.idx:<7} {p.ts - t0:10.6f} {XFER.get(p.xfer, p.xfer):4s} 0x{p.ep:02x} "
              f"{tag} {len(p.data):6d}{st}  {hexs(p.data, args.bytes)}")
        n += 1
        if args.max and n >= args.max:
            break


def cmd_extract(args):
    _, mine, _ = load(args.cap, args)
    ep = int(args.ep, 0)
    with open(args.out, "wb") as f:
        total = 0
        for p in payloads(mine, ep):
            f.write(p.data[args.skip:])
            total += max(0, len(p.data) - args.skip)
    print(f"wrote {total} bytes from ep 0x{ep:02x} to {args.out}")


def _shapes(mine, hb):
    """(ep, len-bucket, first hb bytes) for small OUT/IN transfers = command-ish."""
    s = collections.Counter()
    for p in payloads(mine):
        if len(p.data) <= 512:
            s[(p.ep, p.data[:hb].hex())] += 1
    return s


def cmd_diff(args):
    _, a, _ = load(args.a, args)
    _, b, _ = load(args.b, args)
    sa, sb = _shapes(a, args.bytes), _shapes(b, args.bytes)
    print(f"command-sized transfers keyed by (ep, first {args.bytes} bytes)\n")
    print("only in B (start/stop candidates when A=bring-up, B=stream-only):")
    for k, v in sorted(sb.items(), key=lambda kv: -kv[1]):
        if k not in sa:
            print(f"  ep 0x{k[0]:02x} x{v:<4} {hexs(bytes.fromhex(k[1]))}")
    print("\nin both (counts A / B):")
    for k in sorted(set(sa) & set(sb), key=lambda k: -sb[k])[:40]:
        print(f"  ep 0x{k[0]:02x} {sa[k]:5d} / {sb[k]:<5d} {hexs(bytes.fromhex(k[1]))}")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--bus", type=int)
    ap.add_argument("--addr", type=int)
    sub = ap.add_subparsers(dest="cmd", required=True)

    s = sub.add_parser("summary"); s.add_argument("cap"); s.set_defaults(fn=cmd_summary)
    s = sub.add_parser("fw"); s.add_argument("cap"); s.add_argument("--fw", action="append")
    s.add_argument("--show", type=int, default=12); s.set_defaults(fn=cmd_fw)
    s = sub.add_parser("dump"); s.add_argument("cap"); s.add_argument("--ep")
    s.add_argument("--bytes", type=int, default=32); s.add_argument("--max", type=int, default=0)
    s.add_argument("--all", action="store_true", help="include payload-less halves and all control stages")
    s.set_defaults(fn=cmd_dump)
    s = sub.add_parser("extract"); s.add_argument("cap"); s.add_argument("--ep", required=True)
    s.add_argument("-o", "--out", required=True); s.add_argument("--skip", type=int, default=0,
                                                                 help="strip N header bytes per transfer")
    s.set_defaults(fn=cmd_extract)
    s = sub.add_parser("diff"); s.add_argument("a"); s.add_argument("b")
    s.add_argument("--bytes", type=int, default=8); s.set_defaults(fn=cmd_diff)

    args = ap.parse_args()
    args.fn(args)


if __name__ == "__main__":
    main()
