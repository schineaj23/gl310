#!/usr/bin/env python3
"""Build a synthetic USBPcap pcapng that mimics a GL310 bring-up, to self-test gl310cap.py.

Framing invented for the test: 16-byte header {magic 0x51504657, addr, len, seq}
followed by up to 4080 bytes of firmware, on EP 0x02. Plus a separate-command
variant for the audio image: 32-byte command packet, then raw data packets.
"""
import os
import struct
import sys

out, vid_fw, aud_fw = sys.argv[1], sys.argv[2], sys.argv[3]
vid = open(vid_fw, "rb").read()
aud = open(aud_fw, "rb").read()

blocks = []


def blk(t, body):
    pad = (-len(body)) % 4
    L = 12 + len(body) + pad
    return struct.pack("<II", t, L) + body + b"\0" * pad + struct.pack("<I", L)


blocks.append(blk(0x0A0D0D0A, struct.pack("<IHHq", 0x1A2B3C4D, 1, 0, -1)))
blocks.append(blk(1, struct.pack("<HHI", 249, 0, 0)))
t = [1_700_000_000_000_000]
irp = [0x1000]


def usb(ep, xfer, data, completion, status=0, stage=None, bus=1, addr=7):
    hl = 28 if stage is not None else 27
    h = struct.pack("<HQIHBHHBBI", hl, irp[0], status, 0x09, int(completion), bus, addr, ep, xfer, len(data))
    if stage is not None:
        h += bytes([stage])
    t[0] += 50
    raw = h + data
    blocks.append(blk(6, struct.pack("<IIIII", 0, t[0] >> 32, t[0] & 0xFFFFFFFF, len(raw), len(raw)) + raw))


def bulk_out(ep, data):
    irp[0] += 1
    usb(ep, 3, data, False)
    usb(ep, 3, b"", True)


def bulk_in(ep, data):
    irp[0] += 1
    usb(ep, 3, b"", False)
    usb(ep, 3, data, True)


# noise device on same bus
irp[0] += 1
usb(0x81, 1, b"\x00\x01\x02", True, addr=3)
# GET_DESCRIPTOR(device)
irp[0] += 1
usb(0x80, 2, bytes.fromhex("8006000100001200"), False, stage=0)
dd = struct.pack("<BBHBBBBHHHBBBB", 18, 1, 0x200, 0, 0, 0, 64, 0x07CA, 0xC835, 0x100, 1, 2, 3, 1)
usb(0x80, 2, dd, True, stage=3)
# vendor control
irp[0] += 1
usb(0x00, 2, bytes.fromhex("4001000000000000"), False, stage=0)

seq = 0
for off in range(0, len(vid), 4080):
    chunk = vid[off:off + 4080]
    bulk_out(0x02, struct.pack("<IIII", 0x51504657, 0x100000 + off, len(chunk), seq) + chunk)
    bulk_in(0x83, struct.pack("<II", 0xACC, seq))
    seq += 1
for off in range(0, len(aud), 8192):
    chunk = aud[off:off + 8192]
    bulk_out(0x02, struct.pack("<8I", 0xD0A, 0x200000 + off, len(chunk), 0, 0, 0, 0, 0))
    bulk_out(0x04, chunk)
# a bit of "H.264"
es = b""
for i in range(30):
    es += b"\x00\x00\x00\x01\x67" + os.urandom(10) + b"\x00\x00\x00\x01\x68" + os.urandom(4)
    es += b"\x00\x00\x00\x01\x65" + os.urandom(3000)
for off in range(0, len(es), 16384):
    bulk_in(0x81, es[off:off + 16384])
open(out, "wb").write(b"".join(blocks))
print("wrote", out)
