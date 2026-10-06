# How the card is driven

A condensed reference for the AVerMedia GL310 / C835 (Live Gamer Portable Lite,
USB `07ca:c835`). Everything here is implemented in `tools/` and was verified on the
card. The evidence, and the many dead ends, are in
[research/notes/RE.md](../research/notes/RE.md).

## Hardware

| part | role |
|---|---|
| QPixel QL201 SoC with an ARM core | USB device, H.264 encoder, TS mux; runs the QPSOS firmware `qpvidfwusb.bin` |
| Nuvoton NUC100 MCU (I²C `0x15`) | reports the HDMI signal (register `0x1d`), bridges to other chips |
| ADV7441-class HDMI receiver (I²C `0x21`, `0x31`, `0x35`) | must be configured by the host every session |

## USB transport

Vendor-class interface 0, four bulk endpoints:

| EP | use |
|---|---|
| `0x04` OUT / `0x83` IN | command / reply |
| `0x81` IN / `0x02` OUT | DMA data |

Every command starts with an 8-byte header: `op, sub, count (le16), arg (le32)`.
Commands used:

| op | command |
|---|---|
| `0x01` | register read (sub 0) / write (sub 1). A read of *n* registers works; **the batched write (op `0x03`) misplaces values, so don't use it** |
| `0x02` | memory read/write (byte address, shifted right by 2 by the device) |
| `0x07` | ResetArm: sub 0 holds the ARM in reset, sub 1 releases it |
| `0x09` | start a DMA read/write (word addresses) |
| `0x05` / `0x08` | hardware I²C write / read |
| `0x0b` / `0x0c` | software I²C (status `0x08` = ACK, `0x06` = NAK) |

The command channel has no framing: a single unread reply desynchronises everything
after it. Flooding it with commands exhausts the firmware's 16 request buffers and stalls
the device until it's replugged.

## Bring-up, every session

1. **HDMI receiver init** (`gl310i2c --init --shift 0 --go`), with the ARM held in
   reset. It writes slave `0x31` (`f0←10 f1←0f f4←20`) and slave `0x35`
   (`14←1f 15←ec 1c←49 1d←04 5a←01`), then the 22-register `SelectVideoSource(20)` block
   to slave `0x21`. Without it the encoder produces an almost empty stream. Hardware I²C
   writes always report success, so their status means nothing.
2. **Firmware boot** (`gl310init --go`): it sets up the host interface windows, loads
   `qpaudfwusb.bin` at byte `0x100000` and `qpvidfwusb.bin` at `0`, then releases the
   ARM. QPSOS is up about 165 ms later and keeps a log at DRAM `0x06edc0`
   (`gl310log` reads it). DDR needs no training.

## Mailbox

Host → ARM:
- parameters go in `0x6f8, 0x6f4, … 0x6d8`
- write `0x6cc = (task<<16)|1`, then `0x6fc = (task<<16)|cmd`
- wait for bit 0 of `0x6cc` to clear

ARM → host: eight consecutive registers from `0x6b0`, so one read fetches them all:
- `0x6b0` = message
- `0x6b4…0x6c4` = p1…p5
- `0x6c8` = status; bit 0 = message pending, and clearing it acknowledges the message
- `0x6cc` = our outbound busy flag

The parameter registers are shared with the encoder config block, so order matters.

## Starting the encoder (`gl310start`)

1. `SystemOpen` (cmd `0xf1`, p1 `0x80000011`), then `SystemLink` (`0xf2`, p1 `0x01000100`)
2. Property messages (cmd `0x10`, p1 = selector):

   | sel | name | values |
   |---|---|---|
   | `0x0f` | ExternalTriggerToSync | 0, 0 |
   | `0x10` | PTSResetByTrigger | 0, 0, 0 |
   | `0x12` | DeinterlaceMode | 1 |
   | `0x13` | RateControlEx | 120, 0, 8 |
   | `0x14` | **LargeCompressBuffer** | `0x80004a38`, **`0x4a38`** (see below) |
   | `0x16` | AVDiscardControl | 2 |
   | `0x17` | UseSWPTS | 1 |
   | `0x02` | ViuSyncCode | `0xf1f1f1da`, `0xb6f1f1b6` |

3. Config block, written directly to the registers:

   | reg | name | value | meaning |
   |---|---|---|---|
   | `0x6f8` | SystemControl | `0x2101c219` | stream type 1 (TS), profile 2, level 12, CAVLC |
   | `0x6f4` / `0x6dc` | Picture / OutPicResolution | `0x04380780` | `(h<<16)\|w`; anything smaller **crops**, it doesn't scale |
   | `0x6f0` | InputControl | `0x0f5e0608` | |
   | `0x6ec` | **RateControl** | `0x0078xxxx` | low 16 bits = CBR bitrate in kbps |
   | `0x6e8` | VBRBitRate | `0x1f4007d0` | unused while RateControl's vbr = 0 |
   | `0x6e4` | FilterControl | `0x80002000` | |
   | `0x6e0` | GOPLoopFilter | `0xf199001e` | GOP 30 |
   | `0x6d8` | BlockSize | `0x10` | |
   | `0x6d4` / `0x6d0` | AudioControl / Ex | `0x21161100` / `0x520840f4` | |

4. `StartEncoder` (cmd `0x01`).

The two settings that matter most:
- **LargeCompressBuffer.** The Windows driver logs one word, but the firmware stores two:
  enable and size. The bitstream buffer is `size × 96` bytes, clamped to at least 128 KiB.
  Without the size, any frame over 128 KiB overwrites its own beginning, and keyframes
  grow past that within seconds.
- **RateControl.** The Windows session ran at 60000 kbps, which overflows that buffer and
  also loses frames over USB 2.0. 2–40 Mbps is verified clean.

## Receiving frames

- The ARM posts message `0x40` with:
  - p1 = type (`0x83` for compressed)
  - p2 = word address
  - p3 = 1 if the fragment ends at the ring's end
  - p4 = length in words
- DMA-read the fragment, then reply with cmd `0x30`:
  - `0x6f8` = p1
  - `0x6f4` = p4 (words consumed)
  - `0x6f0` = p5 >> 2
  - `0x6ec` = 0
  - `0x6e4` = 0
- Then clear bit 0 of `0x6c8`.
- Don't send the "release" form of the ack (`0x6ec` = 1 with a tag in `0x6f0`). It
  corrupts the stream.
- **Stale notifications:** the ready flag can become visible before the new parameters.
  If p2/p4 equal the previous fragment's, re-read until they change; otherwise one
  fragment is lost.
- Stopping: keep acking until the ARM goes quiet, then send `StopEncoder` (cmd `0x02`),
  then reboot the firmware. Stopping with frames in flight stalls the device.

## The stream

- MPEG-TS, **32-bit word-swapped** on the wire
- PIDs:
  - `0` PAT
  - `0x42` PMT
  - `0x44` H.264
  - `0x45` AAC: declared but never carries data. Tell decoders not to wait for it.
- H.264 High profile, CAVLC, 1920x1080, 29.97 fps, GOP 30. The stream carries its own
  SPS/PPS.
- The 1080i input is deinterlaced to 30p on the card.
