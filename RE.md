# GL310 (07ca:c835) USB command protocol, recovered from AVer330USB.sys

Source: static disassembly of `vendor/AVer330USB.sys` (3.2802.64.40, the checked build),
using the `CUsbCntl_*` functions located through their own debug strings. The DMA and
mailbox behaviour was cross-checked against `captures/gl310-bringup-debugview.log`
(take 2, a clean restart). RVAs are given so every claim can be re-checked.

USBPcap can't see this device's URBs on Windows (see `captures/NOTES.md`), so the
layouts below were read out of the code rather than off the wire.

> **Update 2026-10-05 — the transport and the read opcodes are now confirmed on real
> hardware**, from macOS over libusb, with no firmware loaded. See
> [§ Confirmed on hardware](#confirmed-on-hardware). USBPcap turned out not to be
> needed for this part: the device answers our own commands, so it validates the
> framing itself. Reproduce with `tools/gl310probe.c`.

## Endpoints

`CUsbCntl_Constructor() cmd_wr(1) cmd_rd(3) dma_wr(0) dma_rd(2)`. Pipe *i* is the *i*-th
endpoint of interface 0, and the log confirms `pipe(1) ep(0x4)`.

| pipe | EP     | role   | used for |
|------|--------|--------|----------|
| 0    | `0x02` | dma_wr | bulk payload of a DMA write (firmware, uploads) |
| 1    | `0x04` | cmd_wr | every command below |
| 2    | `0x81` | dma_rd | bulk payload of a DMA read (verify, encoded stream) |
| 3    | `0x83` | cmd_rd | command replies |

## Transport: `CUsbCntl_GenericCmd` (RVA 0x83930)

```
GenericCmd(cmd, cmd_len, reply, reply_len):
    bulk OUT  EP 0x04 <- cmd[0:cmd_len]
    if reply_len: bulk IN EP 0x83 -> reply[0:reply_len]
```
It holds a lock around the pair. There is no extra framing: the reply is the raw bytes.

## Command header (8 bytes, little-endian)

```
struct cmd_hdr {
    u8  op;      // opcode, table below
    u8  sub;     // 0 = read, 1 = write (I2C read/write-then-read: write length)
    u16 count;   // per-op: #registers, read length, or payload byte count
    u32 arg;     // per-op: register, I2C slave address, or 0
};               // followed by op-specific payload
```

## Opcodes

`accessMode(1)` is what the device reports, so register and address values go out **raw**.
In mode 0 the driver would instead OR 0x8000 (registers) or 0x80000000 (memory) into
writes and mask them off for reads.

| op | function (RVA) | command bytes | reply |
|----|----------------|---------------|-------|
| `0x00` | ReadHciRegister (83be0)  | `00 00 01 00` reg:u32 | 1 byte |
| `0x00` | WriteHciRegister (83c90) | `00 01 01 00` reg:u32 val:u8 (9 B) | none |
| `0x01` | RegisterRead (83d70)     | `01 00 01 00` reg:u32 | 4 bytes (u32) |
| `0x01` | RegisterReadEx (840d0)   | `01 00` n:u16 reg:u32 | 4·n bytes |
| `0x01` | RegisterWrite (83e40)    | `01 01 01 00` reg:u32 val:u32 (12 B) | none |
| `0x03` | RegisterWriteEx (83f50)  | `03 01` n:u16 reg:u32 val[n]:u32 (n ≤ 32) | none |
| `0x02` | MemoryRead (841b0)       | `02 00 04 00` waddr:u32 waddr:u32 (12 B) | 4 bytes |
| `0x02` | MemoryWrite (845c0)      | `02 01 04 00` waddr:u32 waddr:u32 val:u32 (16 B) | none |
| `0x05` | I2CWrite (84db0)         | `05 01` len:u16 slave:u32 data[len] | 1 byte |
| `0x07` | ResetArm (84b50)         | `07` run:u8 `00 00 00 00 00 00` | none |
| `0x08` | I2CRead (84c20)          | `08 00` rlen:u16 slave:u32 | rlen+1 (last = status) |
| `0x08` | I2CWriteThenRead (84f50) | `08` wlen:u8 rlen:u16 slave:u32 wdata[wlen] | rlen+1 |
| `0x09` | StartDMAWrite (84810)    | `09 01 08 00 00 00 00 00` armaddr:u32 nwords:u32 (16 B) | 1 byte, **then bulk OUT EP 0x02, 4·nwords bytes** |
| `0x09` | StartDMARead (849b0)     | `09 00 08 00 00 00 00 00` armaddr:u32 nwords:u32 (16 B) | 1 byte, **then bulk IN EP 0x81, 4·nwords bytes** |
| `0x0B` | SWI2CWrite (85320)       | `0b 01` len:u16 slave:u32 data[len] | 1 byte |
| `0x0C` | SWI2CRead (85190)        | `0c 00` rlen:u16 slave:u32 | rlen+1 |
| `0x0C` | SWI2CWriteThenRead (854c0) | `0c` wlen:u8 rlen:u16 slave:u32 wdata[wlen] | rlen+1; **last byte 0x08 = success** |
| `0x14` | GetUSBSpeed (85700)      | `14 00 01 00 00 00 00 00` | 1 byte |

`waddr`/`armaddr` are **32-bit word addresses**: byte address ÷ 4. `MemoryRead/Write`
shift the byte address right by 2 themselves. For DMA the caller passes word units, which
the log confirms: the audio image at byte 0x100000 goes to `Arm(0x40000)`, and 32 KiB
chunks step `Arm` by 0x2000.

Cross-checks against the log: `UsbSendCmd()` prints `data(<first byte>) len(<bytes>)`.
- The 125 ms GPIO poll shows `data(1) len(8)`, which is RegisterRead: op 0x01, 8 bytes.
- The HDMI status poll shows `data(12) len(9)`, which is SWI2CWriteThenRead: op 0x0C, with
  8 header bytes + 1 sub-address byte. `HAL::getI2C_sw` names slave 0x2a and sub 0x1b, so
  the command is `0c 01 rr 00 2a 00 00 00 1b`, where rr is the read length (likely 1).

## Firmware bring-up (as the driver does it)

```
1. ResetArm(run=0)                        07 00 00 00 00 00 00 00
2. QPHCI_ReInit  mode(1) regBase 0x100000 memBase 0 page 0x100000   (HCI/register setup)
3. CQLCodec_InitializeMemory type(1) 512Mb (DDR init via register writes)
4. SetGPIODefaults dir 0 val 0
5. DMA write Arm 0x5634e, 96,492 bytes, from a scratch host buffer (probably a zero-fill
        of the DSP .bss): 0x5634e·4 = 0x158d38 = 0x100000 + 363832, right after the DSP image
6. qpaudfwusb.bin -> byte 0x100000 (Arm 0x40000), 32 KiB DMA writes, each read back
7. qpvidfwusb.bin -> byte 0x0      (Arm 0x0),     32 KiB DMA writes, each read back
8. ResetArm(run=1)                        07 01 00 00 00 00 00 00
9. ~165 ms later the ARM firmware ("QPSOS2") is up
```
In the log, steps 5-8 take about 200 ms. Step 2-4's register writes still need to be read
out of `QPHCI_ReInit` (49d40/4aca0) and `CQLCodec_InitializeMemory`.

## Streaming (from the log)

The ARM posts a mailbox message, the host DMAs the frame out of ARM memory, then acks:
```
ARM->host  cmd 0x40, p1 0x83 (stream type), p2 = ARM address, p4 = length in words
host       DMA read Arm(p2), 131072 B chunks + remainder, swap(1), EP 0x81
host->ARM  REG_TO_ARM_MESSAGE_STATUS(0x1) REG_TO_ARM_MESSAGE(0x30)       (ack)
```
The mailbox registers themselves (`QPFWAPI_SendMessageToARM` 5a8c0,
`QPFWAPI_AckARMMessage` 5ae40, and the ARM->host poll) are the next thing to decode.

## Confirmed on hardware

Run on macOS 26.6.2 (arm64) against the card on the dock, **no firmware loaded**,
using `tools/gl310probe.c` (read-only by default; writes need `--allow-write`).
libusb claims interface 0 with no fight — nothing else binds a class-0xFF interface.

| claim | result |
|---|---|
| `GenericCmd` = bulk OUT `0x04`, bulk IN `0x83`, no framing | **confirmed** |
| header `{u8 op, u8 sub, u16 count, u32 arg}` | **confirmed** |
| `0x14` GetUSBSpeed → 1 byte | **confirmed**, returns `0x03`, stable |
| `0x01` RegisterRead → 4 bytes | **confirmed** |
| `0x01` RegisterReadEx, reply scales `4·count` | **confirmed**, `count=4` → exactly 16 B |
| `count` semantics | **confirmed**: `ReadEx(0x0,4)`'s first 8 bytes equal `Read(0x0)` ‖ `Read(0x4)` |
| replies stay aligned with commands | **confirmed**, repeated identical cmds give identical replies |
| `accessMode(1)` ⇒ addresses go out raw | **consistent**, raw register numbers read fine |
| `0x00` ReadHciRegister → 1 byte | transport works, **returns `0xff` for every register** |
| `0x0C` SWI2CWriteThenRead → rlen+1 | transport works, **I²C transfer fails, status `0x06`** |
| `0x08` I2CWriteThenRead | transport works, returns `00 00` |

### We are talking to live silicon, not a canned reply

A 128-byte `RegisterReadEx` of 32 registers from `0x0`, read twice 5 s apart, differs in
**exactly two words** — `reg[5]` at `0x14` and `reg[7]` at `0x1c`:

```
reg[5] 0x5621cf3f -> 0x90c64456   delta 983,856,407 over 5.085 s  => 193.47 MHz
reg[7] 0x55fe93ff -> 0x909e4d9d   delta 983,546,270 over 5.085 s  => 193.41 MHz
```

Two independent free-running counters on the same ~193.4 MHz clock, wrapping every
22.2 s, while the other 30 registers stay byte-identical. `reg[1] = 0x000b6c7d` is
stable across every read (an ID/version, not a counter), and `reg[11] = 0x45433210`
looks like a hardwired signature.

`RegisterRead(0x0)` and `RegisterRead(0x100000)` return identical values, so the
register window aliases — consistent with the driver's `regBase 0x100000`.

> **Correction.** `reg[1]` at `0x4` was first recorded here as a stable ID because it
> read `0x000b6c7d` on every attempt. It is **not** an ID: it tracks ARM reset state,
> reading `0x000a4040` while the core is held in reset and `0x000b6c7d` when released.
> It is a useful liveness indicator.

### `0x600` control page, read live

```
0x0600 = 0x00000009     0x0618 = 0x0000ef1f     0x06c8 = 0x00000000  mbox ack
0x0610 = 0x00000000  GPIO dir     0x061c = 0xd4000000     0x06cc = 0x00000000  TO_ARM_MESSAGE
0x0614 = 0x00000000  GPIO val     0x0630 = 0x00222200     0x06f8 = 0x00000008
0x0634 = 0x00000006     0x06ac = 0x06000000     0x06fc = 0x0000000a  TO_ARM_STATUS
```

### Registers recovered from the .sys

Found with `tools/sysmap.py`, which indexes the driver by `.pdata` function bounds
plus the debug string each function prints about itself.

| register | source function (RVA) | meaning |
|---|---|---|
| `0x610` | `CQLCodec_SetGPIODefaults` (`0x4c3a0`) | GPIO direction |
| `0x614` | `CQLCodec_SetGPIODefaults` (`0x4c3a0`) | GPIO value |
| `0x6cc` | `QPFWAPI_SendMessageToARM` (`0x5a8c0`) | host→ARM message |
| `0x6fc` | `QPFWAPI_SendMessageToARM` (`0x5a8c0`) | host→ARM status / doorbell |
| `0x6c8` | `QPFWAPI_AckARMMessage` (`0x5ae40`) | ARM message ack |

`SetGPIODefaults` is only 183 bytes and reduces to two register writes:
`RegisterWrite(0x610, this->dir)` then `RegisterWrite(0x614, this->val)`, with the log
showing `dir 0 val 0`.

**GPIO is not what blocks SW-I²C.** Reading `0x610`/`0x614` live shows both are
*already* `0`, so `SetGPIODefaults` would be a no-op. The real reason is that SW-I²C
transfers are serviced by the ARM firmware — see `FIRMWARE.md`.

## The device is currently warm: firmware loaded, ARM halted

Discovered 2026-10-05 while probing from macOS. **This was not expected** — the
working assumption had been that nothing was initialised.

`MemoryRead` (op `0x02`, read-only) of ARM memory returns the firmware images
byte-for-byte. Verified at 15 scattered offsets, **zero mismatches**:

```
vid+0x000000: 18 f0 9f e5     vid+0x05fbdc: 00 10 a0 e3     aud+0x000000: 20 20 80 0f
vid+0x000100: 51 53 4f 53     vid+0x030000: 6e 00 50 13     aud+0x020000: 00 90 c0 23
vid+0x001000: 00 00 00 ea     vid+0x06ed00: 00 00 00 00     aud+0x058cf0: 00 00 f8 00
```

(`vid+0x100` is ASCII `QSOS`.) So **DDR is initialised and both images are resident.**

But the ARM is **not executing**. `gl310probe --memwatch 0x0 0x200000 0x1000`
samples 512 words across 2 MB twice: 463 non-zero, **0 changed**.

### Why

The card is on a **self-powered dock**. The Windows capture session ran on another
host through that dock, and switching hosts never removed power from the card — so
DRAM kept refreshing and retained what the Windows driver wrote hours earlier.
Windows' `dispatchPnpStop`/`dispatchRemove` left the ARM in reset.

This state is **fragile** (unplugging the card from the dock ends it) and
**valuable**: it lets us exercise everything downstream of DDR bring-up without
having solved DDR bring-up. The single remaining step to boot QPSOS is
`ResetArm(run=1)` — one 8-byte command, `07 01 00 00 00 00 00 00`.

Recovery if the image gets damaged does **not** require the DDR sequence: while
power is maintained the memory controller stays up, so the images can be re-pushed
with `StartDMAWrite`. Only a true power cycle costs us the initialised controller.

## DDR bring-up, recovered

From `CQLCodec_InitializeMemory` (`0x4b930`, 2306 B) via
`tools/sysmap.py writes CQLCodec_InitializeMemory`, which reconstructs the
`(register, value)` sequence by tracking `mov dx, <reg>` / `mov r8d, <val>` before
each vtable call. The log shows this called as `type(1) size(512Mb)`.

| register | role | values written |
|---|---|---|
| `0xf14` | DRAM **command port** — 17 writes | `0x00010003` `0x00010004` `0x00020004` `0x00020005`, then `0x00010103` `0x00010004` `0x00020004` `0x00020005` `0x00030005`, then `0x00010103` `0x00010104` `0x00020104` `0x00020105` `0x00030105` |
| `0xf04` | timing | `0x0d03110b` |
| `0xf10` | mode-register data | `0x01240080`, `0x05140080` |
| `0xf08` | config | `0x00000003` (twice) |
| `0xf18` | enable / go | `0x00000001` (twice) |
| `0xf1c` | status — read then written | (from memory) |
| `0xf40` | — | `0x00000002`, `0x00000004` |
| `0x0060` | — | `0x00200020`, `0x001a001a` |
| `0x0044` | — | (from memory) |

The shape is a textbook DDR2/DDR3 bring-up: program timings, write mode registers,
then walk a command sequence (precharge-all / load-mode / refresh) per rank.
The staged `0x0001xxxx → 0x0002xxxx → 0x0003xxxx` field in `0xf14` reads as a
rank/bank or command selector.

**Not yet complete**: several values show as `(from memory)` because they are
derived from the `type`/`size` arguments at run time. Those paths still need
resolving before a cold boot can be reproduced exactly.

### HCI aperture, from `QPHCI_ReInit` (`0x4aca0`)

Called as `mode(1) regBase(0x100000) memBase(0x0) page(0x100000)`. With `mode == 1`
it programs **three 1 MB paging windows** into ARM memory, `i = 0..2`:

```
base[i]  = i * page                       (0, 0x100000, 0x200000)
start[i] = base[i] + 0x4000
end[i]   = start[i] + page - 1
RegisterWrite(0x81c + i*0xc, start[i])
RegisterWrite(0x820 + i*0xc, end[i])
RegisterWrite(0x824 + i*0xc, ...)
RegisterWrite(0x840, ...)                 (twice, at the end)
```

So each window has a 3-register descriptor with a 0xc stride. This aperture is why
`MemoryRead` works at all — Windows left it programmed.

## Boot attempt 2026-10-05: reset released, ARM still does not execute

Tried on the warm device (firmware resident, DDR up). **Result: negative.**

The command was verified byte-exact against the driver first. `CUsbCntl_ResetArm`
(`0x84b50`) builds `[0]=0x07`, `[1]=(run!=0)?1:0`, `count=0`, `arg=0`, 8 bytes, no
reply, then calls `GenericCmd`. So `07 01 00 00 00 00 00 00` is exactly what the
vendor sends for `run(1)`.

**The reset demonstrably takes effect**, from a hold/release cycle:

```
                       reg0x04     0x061c
initial                000b6c7d    d4000000
after ResetArm(run=0)  000a4040    00000000
after ResetArm(run=1)  000b6c7d    00000000
```

But the core is not running. Three independent checks agree:

- `--memwatch 0x0 0x200000 0x1000`: 512 samples, 0 changed.
- dense 128-word contiguous scans at `0x70000`, `0x80000`, `0xf0000`, `0x160000`:
  0 changed at every base.
- SW-I²C still returns status `0x06` for every sub-address.
- no `0x600`-page register moves on its own over a 1 s interval.

Side effects: `0x61c` cleared from `0xd4000000` to `0` on reset assert and **did not
restore** — it may be a latch or enable that now needs re-setting. The firmware
image itself is **undamaged** (re-verified, 12/12 offsets match).

> Caveat on method: the first `memwatch` sampled one word per 4 KB, i.e. ~0.1% of
> words, which cannot prove absence of activity. The dense contiguous scans and the
> functional I²C test are the load-bearing evidence.

### Missing bring-up steps, from `CQLCodec_FWDownloadAll` (`0x587e0`)

The real order, with call targets resolved:

```
call [rax+0x1f0]            <- reset (vtable slot used at both ends)
QPHCI_ReInit                   0x4aca0
CQLCodec_InitializeMemory      0x4b930
CQLCodec_AOSwitch              0x4c240   <- read-modify-write of register 0x50
CQLCodec_VOSwitch              0x4c2f0   <- read-modify-write of register 0x50
CQLCodec_SetGPIODefaults       0x4c3a0
call 0x81a90
call [rax+0x1a8] / [rax+0x1b0]        <- a RegRead + RegisterWrite pair, register computed
call 0x81ab0
CQLCodec_FWDownload (audio)    0x57ec0
CQLCodec_FWDownload (video)    0x57ec0
call [rax+0x1f0]            <- reset release
```

`AOSwitch` / `VOSwitch` are audio-out and video-out routing bits in register `0x50`;
neither is an ARM clock or enable, so neither explains the failed boot.

### Resolved: why it failed. Reset release is the whole start — but it is step 9 of 9

The DebugView log settles it. There is **nothing** between reset release and a live
firmware:

```
6.17893  StartDMARead Arm(0x1a000) len(28080)   <- last verify chunk of the video image
6.18050  BULK Pipe(2) xfered(28080)
6.18293  CUsbCntl_ResetArm() run(1)             <- the only operation
6.34837  CQLCodecLib_InitDevice() QPSOS2        <- 165 ms later, alive
```

No handshake, no start command, no mailbox poke. `CQLCodecLib_InitDevice` does not
start the ARM; it runs after the ARM is already up. (An earlier revision of this file
guessed the start handshake lived there. It does not.)

So `ResetArm(run=1)` is correct *and* sufficient — but only as the last step of a
sequence whose earlier steps must run **while the core is held in reset**. Both of
our failures follow directly:

- **Attempt 1** (`07 01` alone): `reg[1]` at `0x4` already read `000b6c7d`, the
  *released* value. The ARM was never in reset — it had been out of reset and simply
  not running since Windows' `dispatchRemove`. Releasing an already-released core is
  a no-op.
- **Attempt 2** (`07 00` then `07 01`): a correct reset pulse, but steps 2–6 were
  skipped. Asserting reset cleared `0x61c`, so the core restarted against a
  torn-down HCI and DDR configuration.

### The authoritative bring-up recipe, with arguments

Straight from the log (`CQLCodec_FWDownloadAll() checkState(0) verify(1)`):

| # | operation | arguments | status |
|---|---|---|---|
| 1 | `CUsbCntl_ResetArm` | `run(0)` — **hold** | ✅ confirmed working |
| 2 | `QPHCI_ReInit` | `mode(1) bus(0) regBase(0x100000) memBase(0x0) page(0x100000)` | partial — window descriptors known |
| 3 | `CQLCodec_InitializeMemory` | `type(1) size(512Mb)` | mostly — `0xf00` script known |
| 4 | `CQLCodec_AOSwitch` | `(1)` | register `0x50` RMW; **bit mask unknown** |
| 5 | `CQLCodec_VOSwitch` | `(0)` | register `0x50` RMW; **bit mask unknown** |
| 6 | `CQLCodec_SetGPIODefaults` | `dir(0) val(0)` | ✅ fully known (`0x610`, `0x614`) |
| 7 | `CQLCodec_FWDownload` | `start(0x100000) size(363832)` — audio | ✅ fully known |
| 8 | `CQLCodec_FWDownload` | `start(0x0) size(454064)` — video | ✅ fully known |
| 9 | `CUsbCntl_ResetArm` | `run(1)` — **release** | ✅ confirmed working |

Steps 2–6 are all between 1 and 9, i.e. with the core halted. The driver also runs
steps 3–6 once earlier, at `t=4.933`, before `FWDownloadAll`.

### Download mechanics, confirmed from the log

```
StartDMAWrite Arm(0x0)     len(32768) swap(0) sync(1)   -> BULK Pipe(0)  32768 B
StartDMARead  Arm(0x0)     len(32768) swap(0) sync(1)   -> BULK Pipe(2)  32768 B   (verify)
StartDMAWrite Arm(0x2000)  len(32768) ...
...
StartDMAWrite Arm(0x1a000) len(28080)                                   (final chunk)
```

- 32768-byte chunks, each immediately read back and compared (`verify(1)`).
- `Arm()` is a **word** address and steps by `0x2000` per 32 KiB chunk — confirming
  `0x2000 * 4 = 32768`.
- Final chunk: `0x1a000 * 4 = 0x68000 = 425,984`; `+ 28,080 = 454,064` = exactly the
  video image size. Word addressing verified arithmetically.
- Firmware transfers use `swap(0) sync(1)`; stream reads later use `swap(1) sync(0)`.

### Gaps 2 and 3 closed, empirically

The warm card still held the values Windows wrote, so they could be read off the
hardware rather than disassembled.

**`AOSwitch` / `VOSwitch`** (full disassembly of `0x4c240` / `0x4c2f0`) are both
read-modify-write of register `0x50` via slots `0x1a8` (read) and `0x1b0` (write):

```
AOSwitch(on):  on ? (v &= ~0x02) : (v |= 0x02)
VOSwitch(on):  on ? (v &= ~0x04) : (v |= 0x04)
```

The driver calls `AOSwitch(1)` and `VOSwitch(0)`, predicting bit 1 clear and bit 2
set. The live card reads **`0x50 = 0x00000404`** — exactly that. Confirmed.

This also pins two vtable slots: **`0x1a8` = `RegisterRead(reg, &out)`** and
**`0x1b0` = `RegisterWrite(reg, val)`**.

**HCI windows**, read live from the `0x800` block, decode the third descriptor
register as `base[i]`:

```
0x081c=0x00004000  0x0820=0x00103fff  0x0824=0x00000000     window 0
0x0828=0x00104000  0x082c=0x00203fff  0x0830=0x00100000     window 1
0x0834=0x00204000  0x0838=0x00303fff  0x083c=0x00200000     window 2
0x0840=0x90003124                                           final write
also present: 0x0814=0x00001fff  0x0818=0x80000000  0x085c=0x00000004
```

So `QPHCI_ReInit` is fully: `start=base+0x4000`, `end=start+page-1`, `third=base`,
for `i=0..2` with `page=0x100000`, then `0x840 = 0x90003124`.

**DDR block**, read live — resolving the `(from memory)` values:

```
0x0f00=0x03020307  0x0f04=0x0d03110b  0x0f08=0x00000003  0x0f0c=0x02030000
0x0f10=0x05140080  0x0f14=0x00020004  0x0f1c=0x00000c00  0x0f20=0x00000001
0x0f40=0x00000002        and  0x0044=0x00010000
```

Note the `0xf00` DDR block and `0x800` HCI block **still read correct values after an
ARM reset cycle**, so that configuration survives reset and does not need re-running
while power is maintained. `0x60` reads `0` (a self-clearing strobe).

**`0x61c` is not a lost latch.** It was read as `0xd4000000`, then `0x00000000`, then
`0x14000000`, then `0x10000000` across the session. It varies on its own and is live
status, not state we destroyed.

### Boot attempt 2: full sequence, firmware re-downloaded — still no execution

`tools/gl310init.c` implements steps 1, 2, 4, 5, 6, 7, 8, 9 (step 3 skipped per
above). Run with `--go`:

- **The firmware download works.** 363,832 bytes to ARM byte `0x100000` and 454,064
  bytes to `0x0`, in 32 KiB chunks, **every chunk read back and byte-compared, zero
  mismatches.** This validates `StartDMAWrite`, `StartDMARead`, word addressing and
  chunking against real hardware. We can now load firmware ourselves, which removes
  the dependence on the warm state for everything except DDR training.
- `AOSwitch(1)` / `VOSwitch(0)` correctly no-op (`0x50` already `0x404`).
- `ResetArm(1)` after a verified fresh image: **still no execution.** 0/96 words
  changed at each of three bases; SW-I²C still `0x06`.

So "stale mutated DRAM" was *not* the explanation either. Remaining hypotheses, in
rough order of likelihood:

1. **DDR training genuinely matters** beyond the config registers — implement step 3
   properly. DMA reaches DRAM correctly, but the ARM's fetch path may need the
   controller actually trained rather than merely configured.
2. **A separate reset or clock enable.** The driver exposes
   `QPCODEC_DIAG_HW_RESET` distinct from `QPCODEC_DIAG_RESET_ARM`; there may also be
   an ARM clock/PLL gate. Worth finding `HW_RESET`'s implementation.
3. **Address remapping.** The HCI windows map `start = base + 0x4000`, so a DMA to
   "ARM word 0" may physically land at `0x4000` while the core resets to `0x0`.
   Windows used the same mapping, so something must reconcile them — possibly a
   boot-vector or remap register we have not found.
4. The pipe resets the driver does at `dispatchPnpStart` (pipes 1,3,0,2).

### Boot attempt 3: the core will not execute an 8-instruction loop

The decisive test, `tools/gl310armtest.c`. Instead of debugging QPSOS, load a
trivial ARM stub and watch for it running:

```
0x00: E59F0018   ldr r0, [pc, #0x18]   ; r0 = counter address (literal at 0x20)
0x04: E3A01000   mov r1, #0
0x08: E5801000   str r1, [r0]
0x0c: E5901000   ldr r1, [r0]          ; LOOP
0x10: E2811001   add r1, r1, #1
0x14: E5801000   str r1, [r0]
0x18: EAFFFFFB   b   LOOP
```

An ARM core resets into ARM state with MMU and caches off, fetching from
`0x00000000`, so this needs no setup. Two copies were loaded with separate
counters — **stub A at our `0x0`, stub B at our `0x4000`** — specifically to test
whether the `start = base + 0x4000` window mapping offsets what the core fetches.

Stub A's first word was read back as `0xe59f0018`, confirming it is in memory.
Result over 2 s of polling: **neither counter moved.**

This is a strong negative that eliminates several hypotheses at once:

- not firmware corruption or staleness — this is 8 instructions we wrote and verified;
- not firmware complexity or an early crash — the loop cannot fault;
- not mutated DRAM data sections;
- **not the `+0x4000` offset** — stub B did not run either.

The core is released and simply executes nothing.

### `ResetArm` verified, and a proper reset-state instrument found

Because `0x61c` self-varies and `reg 0x04` had looked unreliable, there was briefly
no evidence `ResetArm` did anything at all (it returns no reply). Settled by
differential scan of registers `0x000`–`0xffc`: read twice to identify the 7
self-varying registers (`0x0014 0x001c 0x0084 0x0088 0x0c18 0x0c38 0x0c54`), then
toggle reset and diff only the stable ones.

**65 registers respond, cleanly and reversibly:**

| | `0x0400` | `0x0404`–`0x04fc` (64 regs) | `0x0004` |
|---|---|---|---|
| held (`run=0`) | `0x43` | `0x00000003` | `0x000a4040` |
| released (`run=1`) | `0x40` | `0x000000dc` | `0x000b6c7d` |

So `ResetArm` works exactly as documented, and **`0x400`–`0x4fc` is the ARM
reset-state block** — bits 0–1 of `0x400` set while halted. The driver never writes
this range (checked across every function), so it is read-only status maintained by
the USB front-end. It is the reliable instrument for "is the core held or released";
prefer it over `reg 0x04`, which tracks the same thing but was observed latched at
the held value for a while after the image was clobbered.

Note that once released, the 64 status registers sit **static at `0xdc`** — the core
is out of reset but idle, not spinning through a fault handler.

### `QPHCI_PowerUp` — checked, not the blocker

`QPHCI_PowerUp` (`0x498f0`) reads register `0x50`, clears bit `0x100` and writes it
back, so "pad control" is also register `0x50`. The live card reads `0x50 = 0x404`,
i.e. bit 8 already clear: the pads are already powered up. Completing that register:

```
bit 1 (0x002)  audio out      (AOSwitch)
bit 2 (0x004)  video out      (VOSwitch)
bit 8 (0x100)  pad power-down (QPHCI_PowerUp clears, PowerDown sets)
bit 10 (0x400) unknown, currently set
```

### Where this points

The core is demonstrably released but executes nothing, even from a verified
8-instruction loop at address 0. So the blocker is not the image and not the reset
line — it is that **the core cannot fetch from DRAM.**

That focuses everything on the one step deliberately skipped: **DDR training**
(`CQLCodec_InitializeMemory`). DMA reaches DRAM correctly, but the DMA engine and
the core's instruction-fetch path are different masters on the memory controller. A
controller that is *configured* but never *trained* can plausibly serve the DMA port
while failing core fetches.

Next, in order:

1. Implement step 3 fully — the `0xf14` command sequence and the rest of the `0xf00`
   block, with `type=1`/`size=512Mb` constant-folded. Then re-run the stub test:
   `0x400`'s block plus the counter give an unambiguous oracle.
2. If that fails, find `QPCODEC_DIAG_HW_RESET`'s implementation (a second reset,
   distinct from `RESET_ARM`) and any ARM clock or PLL gate.

### What remains for a cold boot

Only three gaps, all small and all in hand:

1. the `(from memory)` values in `CQLCodec_InitializeMemory` (derived from
   `type`/`size`, which we know: `type=1`, `size=512Mb`);
2. the third register of each `QPHCI_ReInit` window descriptor, and the `0x840`
   writes;
3. the bit masks in `AOSwitch(1)` / `VOSwitch(0)` on register `0x50`.

Everything else is confirmed. Note this path does **not** depend on the warm state —
it is a full cold bring-up, so it also removes our dependence on dock power.

## Still to do

- Finish DDR bring-up: resolve the `(from memory)` values in
  `CQLCodec_InitializeMemory` that derive from `type`/`size`, and the third
  register of each `QPHCI_ReInit` window descriptor. Needed for a **cold** boot;
  not needed while the card stays powered.
- Release the ARM (`ResetArm(run=1)`) and see whether QPSOS boots from the image
  already in DRAM. If it does, SW-I²C, HDMI status, the mailbox and possibly the
  firmware's debug shell all become reachable at once.
- Why `ReadHciRegister` returns `0xff` — presumably HCI is dark until `QPHCI_ReInit`.
- Why `0x0C` SW-I²C returns status `0x06`; decode `QPPFMGetAttr` in the firmware.
- The meaning of the 1-byte reply to the DMA command (status? ready?).
- The encoder start/stop message codes (`CEncoderTask_*` / `QPFWENCAPI_*`).
- The ARM→host message/status registers (we have the host→ARM side).
