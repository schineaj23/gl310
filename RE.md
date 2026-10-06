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

> **Update 2026-10-05, later — the firmware boots.** QPSOS comes up fully in about
> 8 ms and is running now, and it keeps a readable log in DRAM that narrates its own
> boot. Read it with `tools/gl310log.c`. This **retracts** the earlier conclusion
> that the ARM core executed nothing; see
> [§ The firmware runs](#the-firmware-runs--proved-by-its-own-log) for what the three
> measurement errors were. DDR training is *not* the blocker and should not be
> re-run on the warm card.

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

> **RETRACTED — these cross-checks were never real.** This paragraph used to claim that
> `UsbSendCmd()` log lines showed `data(1) len(8)` for the GPIO poll and `data(12) len(9)`
> for the HDMI status poll, and concluded the latter was op `0x0C`. **The capture
> contains zero `UsbSendCmd` lines and zero `data(N) len(N)` lines of any kind** — the
> string exists in the driver binary but that verbosity was never enabled. I wrote those
> bullets in an earlier session and then reasoned from them for days. Check with:
> `grep -ci usbsendcmd captures/gl310-bringup-debugview.log` → 0.
>
> The conclusion happened to be right, but by luck, and it is now established properly:
> see [§ The I²C transports, resolved](#the-i2c-transports-resolved).

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

> **Correction, later the same day.** Do not trust the `0x400`–`0x4fc` block. In
> subsequent runs it read `0x00000000` regardless of reset state, so the readings
> above are not reproducible and the "out of reset but idle" reading was wrong on
> both counts. Use the firmware's log and the `0x064dc4` tick counter as the
> liveness oracle instead.

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

### RETRACTED: "the core cannot fetch from DRAM"

> **This section's original conclusion was wrong and is kept only so the error is
> on the record.** It claimed the core was released but executed nothing, and that
> DDR training was therefore the blocker. Both claims are false. QPSOS boots, and
> boots cleanly, in about 8 ms. See
> [§ The firmware runs — proved by its own log](#the-firmware-runs--proved-by-its-own-log).

Three mistakes produced the wrong answer, all of them measurement errors rather
than wrong reasoning about the hardware:

1. **The stub test destroyed the boot header.** `gl310armtest` wrote 1 KiB of
   mostly-zero padding over address 0, which includes **offset `0x100`, the ASCII
   `QSOS` image header**. The loader validates that header, so it refused to start
   anything. The stub never ran because the image was no longer bootable — not
   because the core cannot fetch.
2. **The liveness oracle was a single word in the wrong place.** The counter sat at
   `0x080000`, which the running firmware never touches. QPSOS's live data is at
   `0x064a90`, `0x06edb0`+ and across `0x234000`–`0x30d700`.
3. **The `0x400`–`0x4fc` block is not a reliable reset oracle.** It read `0` in
   later runs regardless of reset state. Use the firmware's own log and tick
   counter instead.

The DDR controller was never the problem: it is *already trained*. The live values
`0x0f14 = 0x00020004` and `0x0f18 = 1` are exactly the post-training result (see
[§ `CQLCodec_InitializeMemory`, decoded](#cqlcodec_initializememory-decoded)), so
re-running step 3 would at best be a no-op and at worst cost us the warm state.

Two further hypotheses were tested and are also dead:

- **The HCI `+0x4000` aperture does not translate host addresses.** `tools/gl310aperture.c`
  moved window 0's `start` (`0x81c`) from `0x4000` to `0x8000`, confirmed the register
  took the write, and found our view of DRAM did not move at all. The
  `0x81c`/`0x820`/`0x824` descriptors are simply not in the host access path. The
  registers were restored and verified.
- **Register `0x00` bit 13** — a step invisible in the log, see
  [§ The hidden steps](#the-hidden-steps-the-log-could-not-show) — is already clear
  on the live card (`0x00 = 0x03ff0300`), and `QPHCI_PowerUp` is a no-op on this
  board, so neither was ever gating the core.

## The firmware runs — proved by its own log

The decisive test was read-only. `tools/gl310life.c` snapshots a wide span of DRAM
by DMA, waits, snapshots again, and diffs. It writes nothing, so it cannot cost us
the resident image. Over a 4 MB span with the core simply left alone:

```
$ ./gl310life --go --no-reset --len 0x400000 --settle 2000
   56 of 1048576 words changed  (0x064b24 .. 0x3088f4)
VERDICT: the ARM core IS executing.
```

**QPSOS is running, and has been all along.** The changed regions are the firmware's
`.data`/`.bss` just above the image, plus heap and per-task structures spread over
`0x234000`–`0x30d700` — a populated RTOS, not a crash.

Better still, QPSOS keeps a **log buffer in DRAM** at `0x06edc0`, immediately above
the image (`qpvidfwusb.bin` is `0x6edb0` bytes). Records are 16 bytes of header plus
NUL-terminated text:

| offset | meaning |
|--------|---------|
| `+0x00` | link/length |
| `+0x04` | 0 |
| `+0x08` | timestamp, in ticks of the ~193.4 MHz counter at `0x064dc4` |
| `+0x0c` | source line number |
| `+0x10` | text, tagged `(T)` trace, `(E)` error, `(W)` warn, `(I)` info |

`tools/gl310log.c` reads and follows it. The full boot, verbatim from the card:

```
[    127910:338  ] (E)usbFlashCFI=0
[    223529:340  ] (T)create usb workqueue...
[    244323:340  ] (T)usb_msg_proc inited
[    250670:340  ] (T)Keep the config, host will not see the change
[    253265:340  ] (T)USB is previously configed
[    274058:340  ] (T)USB: HW is DualSpeed
[   1133458:340  ] (T)register -> Q.cam gadget(FX2)
[   1141439:340  ] (T)ep1in bound @ 81
[   1144007:340  ] (T)ep3in bound @ 83
[   1146532:340  ] (T)ep2out bound @ 02
[   1148912:340  ] (T)ep4out bound @ 04
[   1166907:340  ] (T)ql300_set_config = 1
[   1174792:340  ] (T)Enable ep1in, fifo(1) type=bulk, mps=512
[   1198941:340  ] (T)enqueue req 1 to ep4out        ... through req 16
[   1446048:308  ] (T)CODEC_Start HCI Thread
[   1452553:308  ] (T)CODEC_SYS config:2 HIU isr
[   1455576:308  ] (T)CODEC_SYS config:1 dynamic mem alloc
[   1527175:308  ] (T)CODEC_Start M2M Thread
[   1533338:308  ] (T)CODEC_Start DTM Thread
[   1538166:308  ] (T)CODEC_Start VDCM Thread
[   1543842:308  ] (T)Start Update Tick Thread
```

1,543,842 ticks at 193.4 MHz is **8.0 ms from reset release to all threads up**. The
only error is `usbFlashCFI=0`, which is expected: there is no boot flash, which is
why the image comes over USB in the first place.

What this settles:

- The boot sequence we implemented is **sufficient**. Steps 1–9 minus DDR training
  produce a fully booted firmware.
- `ResetArm` really does halt and restart the core. Holding then releasing it makes
  the log replay from the top (148 records before, 189 after).
- **`Q.cam gadget(FX2)` is confirmed**, no longer a strings-only guess — and the
  endpoint bindings it prints (`ep1in@81 ep3in@83 ep2out@02 ep4out@04`) are exactly
  the pipe map in [§ Endpoints](#endpoints). So after boot, *the ARM firmware itself
  is the USB device we are talking to*; before boot it is a loader in ROM.
- `0x064dc4` is a free-running ~193.4 MHz tick counter maintained by the firmware's
  "Update Tick Thread" — a cheap, unambiguous heartbeat for all future work.

### The hidden steps the log could not show

`CQLCodec_FWDownloadAll` (`0x587e0`) is the orchestrator. The DebugView log only
shows functions that print, so reading the function itself revealed two steps that
were invisible on the wire, plus every delay:

```
ResetArm(0)                                  [slot +0x1f0]
QPHCI_ReInit(hci)
CQLCodec_InitializeMemory(this)
CQLCodec_AOSwitch(this, this+0x348)
CQLCodec_VOSwitch(this, this+0x338)          [0x4c2f0]
CQLCodec_SetGPIODefaults(this)
QPTMDelayMilliS(50)
v = RegisterRead(0x0000); v &= ~0x2000; RegisterWrite(0x0000, v)   <-- not logged
QPTMDelayMicroS(1)
FWDownload(audio -> 0x100000)
QPTMDelayMilliS(1)
FWDownload(video -> 0x0)
QPTMDelayMicroS(500)
ResetArm(1)
QPTMDelayMilliS(150)
```

`0x81a90` is `QPTMDelayMilliS`, `0x81ab0` is `QPTMDelayMicroS`. The 150 ms tail
matches the log's ~165 ms to the next line. Register `0x00` bit 13 is cleared and
never restored; on our warm card it is already clear.

Accessor slots, resolved. The codec object embeds the HCI object at `+0x100`, so a
codec slot at `+0x1XX` and an HCI slot at `+0xXX` are the same pointer:

| slot | signature | is |
|------|-----------|-----|
| `+0xa0` / `+0x1a0` | `(hci, u8 reg, u8 val)` | HCI register write (op `0x00`) |
| `+0xa8` / `+0x1a8` | `(hci, u16 reg, u32 *out)` | RegisterRead (op `0x01`) |
| `+0xb0` / `+0x1b0` | `(hci, u16 reg, u32 val)` | RegisterWrite (op `0x01`) |
| `+0xc8` / `+0x1c8` | `(hci, u32 byteaddr, u32 *out)` | MemoryRead (op `0x02`) |
| `+0xd8` / `+0x1d8` | `(hci, u32 byteaddr, u32 val)` | MemoryWrite (op `0x02`) |
| `+0xf0` / `+0x1f0` | `(hci, int run)` | ResetArm (op `0x07`) |

Register accessors take the index in `dx` (16-bit); memory accessors take a full
32-bit **byte** address in `edx` and shift it right by 2 themselves.

### `CQLCodec_InitializeMemory`, decoded

Not needed on the warm card, but required for a cold boot, so it is recorded here.
`0x4b930`, driven by four config fields. Which branch this board takes is pinned by
five independent live register values:

| field | value | how we know |
|-------|-------|-------------|
| `+0x3c0` type | `1` | logged: `type(1)` |
| `+0x3c4` size | `0x200` (512 Mb) | logged: `size(512Mb)` |
| `+0x3c8` | `8` | `0xf10 = 0x05140080` rules out 4; the autodetect branch requires exactly 8; `QPHCI_PowerUp` early-returns on 8, matching its no-op behaviour |
| `+0x3cc` | `0x10020` | only value that yields the live `0xf40 = 2` |

So the board runs the **geometry-autodetect** path, which is why the live
`0xf14 = 0x00020004` is *not* the hardcoded `0x20005` that `size == 0x200` would
give. In order:

```
cols = 7; rows = 2
RegisterWrite(0xf14, (rows << 16) | cols)
MemoryWrite(0, (rows << 16) | cols)
while (cols > 3) { MemoryWrite(1 << (cols + 6), cols - 1); cols--; }
cols = MemoryRead(0) & 0xf                     /* aliasing reveals the real width */
RegisterWrite(0xf14, (rows << 16) | cols)
MemoryWrite(0, rows)
while (rows > 1) { MemoryWrite(1 << (cols + 0x15), rows - 1); rows--; }
rows = MemoryRead(0) & 0xf
RegisterWrite(0xf14, (rows << 16) | cols)      /* -> 0x00020004 on this board */
v = RegisterRead(0xf1c); RegisterWrite(0xf1c, v & ~0x300)
RegisterWrite(0xf04, 0x0d03110b)
RegisterWrite(0xf08, 0x00000003)
RegisterWrite(0xf40, 0x00000002)               /* type==1 && 0x3cc==0x10020 */
RegisterWrite(0xf10, 0x05140080)               /* 0x3c8 != 4 */
RegisterWrite(0xf18, 0x00000001)
QPTMDelayMilliS(100)
```

It probes address aliasing to find the real row/column width: write a value at 0, then
a distinct marker at each candidate address line, and read address 0 back — whichever
marker landed there tells you how many address bits physically exist. Note the probe
runs *before* the timing registers are programmed, i.e. at reset-default timings.

`0xf00 = 0x03020307` and `0xf0c = 0x02030000` are **never written by the driver** at
all, so they are hardware reset defaults and need nothing from us.

### What remains for a cold boot

Nothing substantive. Every step is now written out:

1. `CQLCodec_InitializeMemory` — fully decoded above, including the autodetect loop
   and all four config fields.
2. `QPHCI_ReInit` — fully decoded, including the third register of each descriptor
   and both `0x840` writes (`0x70003124` then `0x90003124`).
3. The hidden `register 0x00 &= ~0x2000` step and every delay.

The one untested part is DDR training itself, because the card has stayed powered and
re-running it on a trained controller would risk the warm state for no gain. It should
be exercised the first time the card is cold-booted, with `gl310log` watching.

## The mailbox — decoded, and confirmed working

> Earlier notes had `0x6cc` and `0x6fc` the wrong way round. The log format string in
> `QPFWAPI_SendMessageToARM` settles it: **`0x6cc` is `REG_TO_ARM_MESSAGE_STATUS`,
> `0x6fc` is `REG_TO_ARM_MESSAGE`.**

### Registers

Host → ARM:

| register | role |
|----------|------|
| `0x6f8` `0x6f4` `0x6f0` `0x6ec` `0x6e8` `0x6e4` `0x6e0` `0x6dc` `0x6d8` | params 1..9, **descending** |
| `0x6cc` | `REG_TO_ARM_MESSAGE_STATUS` = `(taskId << 16) \| (needAck << 8) \| 1` |
| `0x6fc` | `REG_TO_ARM_MESSAGE` = `(taskId << 16) \| cmd` |

ARM → host (read by the unnamed function at `0x5ab10`, which needs `this+0x25c == 2`):

| register | role |
|----------|------|
| `0x6b0` | message word |
| `0x6c8` | status word |
| `0x6b4` `0x6b8` `0x6bc` `0x6c0` `0x6c4` | params 1..5 |

### Sequence

```
1. MailboxReady: poll 0x6cc until bit 0 is clear        (0x5b020, 500 ms timeout)
2. write params, 0x6f8 downwards
3. write 0x6cc = (taskId<<16) | (needAck<<8) | 1        <- does NOT trigger
4. write 0x6fc = (taskId<<16) | cmd                     <- THIS triggers the ARM
```

**Confirmed on hardware**, with `tools/gl310mbox.c`:

```
  0x6cc <- 0x00000001   (status, doorbell set)
    0x6cc reads 0x00000001, doorbell=1  (status alone does not trigger)
  0x6fc <- 0x00000004   (cmd 0x04, task 0)  <- this is the trigger
    *** the ARM cleared the doorbell after 0 ms - it consumed the message
```

Step 3 and step 4 had to be separated to prove this. Writing only the status leaves
bit 0 set indefinitely — verified by writing `0xabcd0001` and reading it back
unchanged three times. Writing the message register makes the ARM consume it and
**zero `0x6cc` entirely** (not just bit 0). So bit 0 of `0x6cc` is the busy flag, and
`0x6fc` is the doorbell.

Corroboration that the register roles are right: on first contact `0x6fc` read `0x0a`,
i.e. `SetAudioInputVolume` — plausibly the last thing the Windows driver ever sent.

`QPFWAPI_SendMessageToARM` also calls slot `+0xf8` with the constant 1 after writing
both registers. Its purpose is still unidentified (it is the one accessor slot we
cannot place), but it is evidently **not required** — the ARM consumes messages
without it.

### Command codes

Recovered mechanically from the `QPFW*API_` wrappers: each builds its message word as
`and eax, 0xffff0000` / `or eax, <code>`. Command `0x10` is a generic *set property*,
with the property selector in param 1.

| cmd | wrapper | params |
|-----|---------|--------|
| `0x01` | `StartEncoder` | none |
| `0x02` | `StopEncoder` | p1, p2 |
| `0x03` | `PauseEncoder` | none |
| `0x04` | `ResumeEncoder` | none |
| `0x07` | `GetViosdTableaddr` | p1, p2 |
| `0x08` | `GetCurVidBufInfo` | p1 |
| `0x0a` | `SetAudioInputVolume` | p1 |
| `0x0c` | `InsertUserData` | p1 |
| `0x10` | *set property* | p1 = selector, p2.. = value |
| `0x11` | `SetEncMode` | p1, p2, p3 |
| `0x12` | `GetAFrame` | none |
| `0x81` | `StartDecoder` | p1..p6 |
| `0x82` | `StopDecoder` | p1..p4 |
| `0x86` | `Flush` | p1 |
| `0x87` | `GetPlayInfo` | p1 |
| `0x88` | `SetAudioOutputVolume` | p1 |
| `0xb0` | `GetVouOsdMem` | p1, p2 |
| `0xf1` | `SystemOpen` | p1 |
| `0xf2` | `SystemLink` | p1 |
| `0xf3` | `SystemClose` | none |

Property selectors for command `0x10`:

| sel | property | sel | property |
|-----|----------|-----|----------|
| `0x01` | IndexCapture | `0x0c` | MJPEGFrameBuffer |
| `0x02` | ViuSyncCode | `0x0f` | ExternalTriggerToSync |
| `0x03` | MP4VideoBlockNumber | `0x10` | PTSResetByTrigger |
| `0x04` | AudioEnhancement (p2..p9) | `0x11` | RawVideoDecimation |
| `0x05` | EnableVidPadding | `0x12` | DeinterlaceMode |
| `0x07` | VBIInfo (p2..p6) | `0x13` | DeinterlaceMode (long form) |
| `0x09` | FreezeVideo | `0x14` | LargeCompressBufferControl |
| `0x0a` | StillVideoInput | | |
| `0x0b` | MJPEGQuality | | |

The ack direction (`QPFWAPI_AckARMMessage`, `0x5ae40`) fires only when bit 8 of the
incoming message is set, and replies with code **`0x31`** when the incoming command is
`< 0x80` (encoder) or **`0xa2`** when `>= 0x80` (decoder), echoing the incoming message's
high 16 bits.

### Encoder start, as the driver does it

`CEncoderTask_Start` (`0x77cf0`):

```
CQLCodec_UpdateMiscConfig(codec, hTask)        (0x56e20)
MailboxReady(codec, 500)                       takes the mailbox lock
CQLCodec_UpdateEncoderConfig(codec, hTask, 0)  (0x577b0) - issues the property sets
QPFWENCAPI_StartEncoder(codec, hTask)          cmd 0x01
release the mailbox lock                       (0x5b130)
```

So `CQLCodec_UpdateEncoderConfig` is the remaining piece of real work: it is where the
`0x10`/`0x11` property messages get their actual values.

### I²C, resolved

`tools/gl310probe --raw "0c 01 01 00 2a 00 00 00 1b" 2` still returns status `0x06`.
The firmware says why, in its own string table:

```
i2c.c:QPSOSI2CTransfer: Software (GPIO) I2C not supported
```

So **op `0x0C` (SW-I²C) will never work on this build** — use op `0x08` (HW I²C),
which answers. That closes a question that had been open since the first probe.

### A caveat on `MemoryRead`

Two op-`0x02` reads of the same address inside one USB session returned identical
values for the firmware's tick counter even ~100 ms apart, while reads from separate
invocations advanced correctly. Treat consecutive `MemoryRead`s of one address as
possibly latched, and prefer DMA reads (`gl310life`) when a value must be fresh. The
818 KB byte-exact firmware verify went through DMA, so that result is unaffected.

## It works: H.264 captured from the card

**The encoder runs and produces a valid stream.** `tools/gl310start.c` performs the
sequence below and captured 95,128 bytes that **ffmpeg independently identifies**:

```
Stream #0  h264  (H.264 / AVC / MPEG-4 AVC)  stream_type 0x1b  PID 0x44
Stream #1  aac                               stream_type 0x0f
ts_packetsize=188   start_pts=6010
```

Saved as `captures/first-capture-1080p.ts`. 505 of 505 TS packets carry sync `0x47`.

**The output is MPEG-2 Transport Stream, 32-bit byte-swapped on the wire.** That is
what `swap(1)` means in the driver's `CQLCodec_StartDMARead` calls. Un-swap each
4-byte word and the TS appears immediately — PAT on PID 0, PMT on `0x42`, elementary
stream on `0x44`. So the card does not emit raw H.264; it emits a muxed TS with AAC
audio alongside, which is why `SystemControl = 0x2101c219` and the audio config
registers matter even for video-only use.

Only the first fragment was captured, so ffmpeg reports `width=0` — there is no SPS
in it. That needs continuous capture, see below.

### The sequence, verbatim from the working session

Every value comes from `captures/gl310-bringup-debugview.log`, i.e. from this card
actually working at 1920x1080, so none of it is guessed.

```
cmd 0xf1  SystemOpen   p1 = 0x80000011
cmd 0xf2  SystemLink   p1 = 0x01000100
          (4 bits per field: vi=0 vic=0 vo=1 voc=0 ai=0 aic=0 ao=1 aoc=0)
cmd 0x10  sel 0x0f  ExternalTriggerToSync   0, 0
cmd 0x10  sel 0x10  PTSResetByTrigger       0, 0, 0
cmd 0x10  sel 0x12  DeinterlaceMode         1
cmd 0x10  sel 0x13  RateControlEx           120, 0, 8
cmd 0x10  sel 0x14  LargeCompressBuffer     0x80004a38
cmd 0x10  sel 0x16  AVDiscardControl        2
cmd 0x10  sel 0x17  UseSWPTS                1
cmd 0x10  sel 0x02  ViuSyncCode             0xf1f1f1da, 0xb6f1f1b6
```

then eleven **direct register writes** — no message — which is the whole of
`CQLCodec_UpdateEncoderConfig`:

| register | value | field |
|----------|-------|-------|
| `0x6f8` | `0x2101c219` | SystemControl |
| `0x6f4` | `0x04380780` | PictureResolution — `0x780`=1920, `0x438`=1080 |
| `0x6f0` | `0x0f5e0608` | InputControl |
| `0x6ec` | `0x0078ea60` | RateControl |
| `0x6e8` | `0x1f4007d0` | VBRBitRate |
| `0x6e4` | `0x80002000` | FilterControl |
| `0x6e0` | `0xf199001e` | GOPLoopFilter |
| `0x6d8` | `0x00000010` | BlockSize |
| `0x6dc` | `0x04380780` | OutPicResolution |
| `0x6d4` | `0x21161100` | AudioControl |
| `0x6d0` | `0x520840f4` | AudioControlEx |

and finally `cmd 0x01 StartEncoder`.

Two things worth noting. The config block occupies the **same registers as the mailbox
parameters** — one shared scratch window — which is why ordering matters: the ARM
consumes each property message before the next write lands. And there is **no
`UpdateConfig` (cmd `0x06`)** between the config writes and `StartEncoder`, so the ARM
reads those registers while processing `StartEncoder`.

The register addresses are not immediates in the driver; they live in codec object
fields `0x312`–`0x328`, one `u16` per setter, and each setter
(`QPFWENCAPI_SetSystemControl` at `0x5b610` and the eleven following it) just does
`RegisterWrite(*(u16*)(this+field), value)`.

### Frame delivery

The ARM posts, on `0x6b0`:

```
cmd 0x40  p1 = 0x83 (stream type)  p2 = buffer address in WORDS
          p3 = 0                   p4 = length in WORDS
```

Observed live: `p2=0x601b00` (byte `0x1806c00`), `p4=0x5ce6` = 95,128 bytes. The host
DMA-reads that, then must do **both** of these or the ARM re-posts the same descriptor
forever:

1. `CTask_CompleteArm` (`0x725d0`) sends message code **`0x30`**, echoing p1..p4.
2. `QPFWAPI_AckARMMessage` (`0x5ae40`) finishes by **writing the inbound status word
   back to `0x6c8`**. This is the step that frees the ARM to post the next message.

Doing only (1) is exactly the bug that produced 40 reads of one descriptor in the
first run. The ack reply message (code `0x31` for encoder, `0xa2` for decoder) is only
sent when bit 8 of the incoming message is set, and `cmd 0x40` arrives with it clear.

### Frame reassembly, confirmed by continuity counters

The ARM delivers each frame as **fragments**, and `p3` is the last-fragment flag. The
two message subtypes in `CEncoderTask_ProcessArmMessage` name it: `EncDataOutReq`
(`p3 = 0`, more to come) and **`EncDataOutLastReq`** (`p3 = 1`, end of frame). `p1` is
the stream type (`0x83`; the function also logs `unknown compressed audio type(p1)`),
`p2` is the address in words and `p4` the length in words.

Fragment addresses chain exactly, and each frame's fragments sum to the compressed
buffer size — e.g. `0x5ce6 + 0x1a`, `0x22f + 0x5ad1`, `0xfb + 0x5c05`,
`0x23f2 + 0x390e`, all `= 0x5d00` words = 95,232 bytes.

> **Correction — this section overclaimed.** "Every fragment in arrival order" fits the
> one slow capture it was derived from, but it does **not** generalise, and the
> reassembly is *not* solved. See
> [§ What the faster loop revealed](#what-the-faster-loop-revealed) below. Treat the
> continuity-counter result as evidence that each ring image is internally coherent,
> not as proof that concatenation is correct.

Provisionally, concatenating every fragment in arrival order and un-swapping each
32-bit word yields a transport stream. Measured against TS continuity counters on
PID `0x44`:

```
  step  0:      3   duplicate/stall
  step  1:  11997   correct          <- 99.21%
  step  2..15:  93  lost packets
```

The handful of errors are **losses, not duplicates**, which is the signature of a host
that cannot keep up rather than of wrong reassembly. Two other numbers agree: the
aggregate rate is 8.2 Mbps, matching the configured `VBRBitRate = 0x1f4007d0`
(`0x1f40` = 8000 kbps peak), and the segment-only hypotheses give 2.6–6 Mbps, which
do not.

`tools/gl310start.c --out F` writes a sidecar `F.idx` of
`ms offset len p1 p2 p3 p4 p5` so reassembly can be re-derived offline without holding
the card open.

### The firmware says what the real limit is

After a capture run, `gl310log` grows from 46 lines to 637, every one of them:

```
(E)Drop
(W)can't get sysmsg in dwc_otg_pcd_handle_in_ep_intr
```

That is QPSOS's Synopsys DesignWare USB OTG peripheral driver running out of system
messages in its IN-endpoint interrupt handler, because the host is not draining fast
enough. **This is almost certainly the mechanism behind the wedges below as well** —
once that pool is exhausted the gadget stalls, and a stalled gadget is exactly what we
saw. The two symptoms have one cause: a synchronous, one-transfer-at-a-time host loop
against an 8 Mbps stream.

**What the pool actually is.** At boot QPSOS logs `enqueue req 1 to ep4out` through
`req 16` — sixteen request buffers on `ep4out`, which is our **command** pipe `0x04`,
drained by its HCI thread. So the limit is on **command round trips, not data rate**.
The original loop spent about thirty round trips per fragment at ~100 fragments/s,
roughly 3000 commands/s against a 16-deep queue.

So the first fix is round-trip count, not overlap, and it needs no async machinery:

| per fragment | before | after |
|--------------|--------|-------|
| collect the notification | 8 register reads | **1** `RegisterReadEx(0x6b0, 8)` |
| ack params | 6 register writes | **1** `RegisterWriteEx(0x6e4, 6)` |
| doorbells + status clear | 3 writes | 3 writes |
| DMA command + payload | 2 | 2 |
| per-fragment `printf`, 8 ms sleep | yes | none |
| **total** | **~30** | **9** |

The whole inbound mailbox is eight *consecutive* registers (`0x6b0` message, `0x6b4`–`0x6c4`
params, `0x6c8` inbound status, `0x6cc` outbound doorbell), so one `RegisterReadEx`
replaces eight transfers. Confirmed on the card.

The vendor driver throttles harder still, and this is worth copying: in the captured
session `CTask_ProcessDataStreaming` logged `rd_ready(0) wr_ready(0)` in **1203 of 3653**
calls — a third of the time it declines to issue a DMA at all. It only moves data when
both the ARM has something ready and a host buffer is free.

Beyond that, the asynchronous path is the architecture the vendor driver uses:
`CUsbCntl_StartDMARead` calls `QPUsbInterface_UsbAsyncIo`, frames are read with
`sync(0)` (versus `sync(1)` for the firmware verify), and completion is handled later
in `CEncoderTask_ProcessIoComplete`. `libusb_submit_transfer` with several reads
outstanding is the equivalent.

### Flow control, solved: the ack carries a consumed count

`CTask_CompleteArm` (`0x725d0`) dispatches on the incoming command — `0x40` and `0x41`
both reply with code `0x30` — and its parameters come from a **per-request array at
`task + reqid*0x48`**, not from the incoming message. The captured session prints the
fields with real values:

```
CTask_CompleteArm() type(0x83) addr(0x668f00) size(160740) offset(160740)
                    PTS(715827882) valid(0) last(0) frameFlags(0x0)
```

for an incoming message of `p1=0x83 p2=0x668f00 p4=0x9cf9` words (=160740 bytes) and
`p5=0xaaaaaaaa`. Mapping the fields to the registers the function writes:

| register | field | value |
|----------|-------|-------|
| `0x6f8` | `+0x1d4` type | incoming `p1` |
| `0x6f4` | `+0x1c4` offset `>> 2` | **how much we consumed, in words** |
| `0x6f0` | `+0x1c0` PTS | incoming `p5 >> 2` (`715827882 = 0xaaaaaaaa >> 2`) |
| `0x6ec` | `+0x1d0` valid | `0` |
| `0x6e8` | — | *not written on this path* |
| `0x6e4` | `+0x1e4` | `0` |

`offset` always equals `size` in the log: the host reports it took the whole fragment.
**Putting the address in `0x6f4`, as a blind echo does, tells the ARM nothing about
progress** — which is exactly why its read pointer never advanced and it kept
re-posting ring state until it gave up.

With the consumed count sent properly, the transport layer is **perfect**:

```
863 fragments in 3 s, mean 6731 B    (was 455 fragments of mean 59 KB)
802 of 862 chain exactly: p2_next == p2 + p4
60 wraps to ring base, and p3 == 1 exactly 60 times
PID 0x0044: 30853/30853 continuity steps correct (100.00%), 0 dups, 0 losses
PID 0x0045: 42/42 correct
```

So `p3` marks **the fragment that ends at the ring end**, not end-of-frame, and
reassembly really is "concatenate every fragment in order, un-swapping 32-bit words".
That now holds with zero packet loss rather than 99.21%.

Also settled: the throttle. `--idle-us` must be nonzero — polling flat out at
5381 polls/s produced *nothing at all*, because the firmware's HCI thread never got
scheduled. 1 ms (≈366 effective polls/s) works.

### But there is no video in the stream yet

**The transport stream is flawless and its payload is not compressed video.** Measured
on the elementary stream of PID `0x44`:

```
00 00 00  :  448928     <- forbidden inside a valid H.264 NAL
00 00 01  :   20737     <- should only appear at NAL boundaries
00 00 03  :    4336     <- the emulation-prevention escape
byte entropy: 5.396 bits/byte   (compressed video is ~7.9-8.0)
0x00 is 27% of all bytes, 0xff is 14%
```

`ffmpeg` decodes **0 frames** and reports `data partitioning is not implemented`, which
is it mis-parsing noise as NAL types 2/3/4. Only 7 of 30854 packets carry
`payload_unit_start`, where one PES per frame would give ~90.

**This retracts two earlier claims.** First, that the ADV7441 needs no host I²C setup —
that was inferred from a structurally valid TS, which does not imply pictures. Second,
the original "ffmpeg identifies it as h264 + aac" result: those codec names come from
the **PMT descriptor**, i.e. from what the muxer *declares*, not from decoded video.
The muxer has been running correctly all along with nothing real to carry.

So the remaining work is to give the encoder a picture — see below.

## The HDMI receiver: what's known, and the one thing blocking it

### The init sequence, recovered

`CADI7441_InitDevice` (`0x9ba10`) is short. A 1 s delay, then eight two-byte register
writes across two I²C addresses, then calls to eight object slots:

```
QPTMDelayMilliS(1000)
slave 0x31 <- f0 10      slave 0x35 <- 14 1f
slave 0x31 <- f1 0f      slave 0x35 <- 15 ec
slave 0x31 <- f4 20      slave 0x35 <- 1c 49
                         slave 0x35 <- 1d 04
                         slave 0x35 <- 5a 01
```

`0xf0`/`0xf1`/`0xf4` on the ADV744x family are the IO-map registers that program the
I²C addresses of the sub-maps, so `0x31` is the IO map and `0x35` a sub-map.

### Ordering: this happens BEFORE the firmware is loaded

Easy to get wrong, and I did at first:

```
4.934  CADI7441_InitDevice()
5.937  CADI7441_SelectVideoSource source(20)
5.981  CQLCodec_FWDownloadAll()
6.348  CQLCodecLib_InitDevice() QPSOS2
```

So host-side I²C runs against the **boot loader, with the ARM still in reset** — not
against QPSOS, which owns the bus afterwards and has its own `i2c.c`.

### Addressing and reply format

Addresses are **7-bit**: the log prints `QPCODEC_DIAG_I2C_WRITE_THEN_READ(0x15)` for the
Nuvoton MCU, whose 7-bit address is `0x15` (8-bit `0x2a`). `0x31` and `0x35` are
likewise 7-bit, and both being odd rules out the 8-bit reading.

From `CUsbCntl_I2CWriteThenRead` (`0x84f50`): the reply is **`rlen+1` bytes with the
last byte a status, and `0x08` means success**. That makes ACK and NAK distinguishable,
which is what makes a bus scan meaningful.

### Which transport

`CI2C_Constructor` (`0xa5310`) installs three slots per type, and the session builds
two objects, `type(1)` and `type(6)`:

| type | slot `+0x38` reaches | USB opcode |
|------|----------------------|------------|
| 1 | `CUsbCntl_I2CRead` | `0x08`, HW I²C |
| 6 | `CUsbCntl_SWI2CRead` | `0x0c`, SW I²C |

The MCU is on SW-I²C (the `data(12) len(9)` poll), so the ADV7441 is on **HW I²C, op
`0x08`**.

### The blocker: the I²C master never answers

`tools/gl310i2c.c --scan` walks all 256 slave values and reports any status != 0.
Result, in **both** states and on **both** transports:

| transport | ARM held | firmware up |
|-----------|----------|-------------|
| op `0x08` HW | status `0x00` on all 256 | status `0x00` on all 256 |
| op `0x0c` SW | status `0x06` on all 256 | status `0x06` on all 256 |

Uniform, address-independent failure. `0x06` matches the firmware's own
`QPSOSI2CTransfer: Software (GPIO) I2C not supported`, and the loader evidently
refuses it too. `0x00` on the HW path looks like the command is accepted but the
master does nothing.

**The `QPHCI_Init` hypothesis is wrong.** Its extra 2800 bytes over `ReInit` are
thread creation (`EMU_ThreadProc`), `QPHCI_PowerUp` and a `QLCODEC_REG_CHIP_VERSION`
read — no I²C clock or enable. Both functions write the same window descriptors and
`0x840` twice, and nothing else by immediate register number.

**The command encoding is not wrong either.** `CUsbCntl_I2CRead` (`0x84c20`) builds
exactly `08 00 rlen:u16 slave:u32`, command length 8, reply `rlen+1`, with the slave a
zero-extended **byte** — which is what `gl310i2c` sends, and the scan covers all 256
byte values.

**The real reason is that the running firmware has no I²C at all.** Its USB gadget
implements only three commands, and the whole set is visible in its string table:

```
ql300 got cmd HIU_REG %s, wLong %d, dwAddr 0x%08X
ql300 got cmd RESET_ARM %s
ql300 got cmd IIC_W_Multi not impl
unknown cmd: %08X %08X : %d
```

Register access and ARM reset, and I²C explicitly *not implemented*. So with
`qpvidfwusb.bin` running, op `0x08` and op `0x0c` can never work — matching the
uniform `0x00` and `0x06` statuses exactly. The vendor driver does its ADV7441 I²C at
t=4.93 s, **before** this image is ever downloaded, so it is talking to something else:
the boot loader the chip comes up in.

## The video input is an IT6604 behind the MCU — not the ADV7441

This reframes the whole input path, and it comes from the log:

```
6.34886  InterfaceNUC100::InterfaceNUC100 entry, (slave_addr = 0x2B)
6.34888  InterfaceNUC100::InterfaceNUC100 Info: Is ISP mode ? (0)
6.55907  InterfaceNUC100::getHdmiVideo_6604 info : monitor match (WAct x HAct = 1920 x 540)
```

The HDMI receiver is an **IT6604**, and the host does not drive it directly — it goes
through the **Nuvoton NUC100 MCU**, whose method is literally named
`InterfaceNUC100::accessRegs_viaNUC`. The class has the whole input API:
`getHdmiVideo_6604`, `getHdmiAudio_6604`, `getColorConversion_6604`, `getHdmiVideo_HDCP`,
`setupHdmiVideo_ex`, `getEDIDMode` / `setEDIDMode` / `notifyEDIDMode`, `getVersion`,
`checkDevice`, `setTI3101Volume`.

`1920 x 540` is 1080i — the camera's signal, seen and measured by the working driver.

Note the timing: `InterfaceNUC100` is constructed at **6.349 s, after** the firmware
boots, unlike the ADV7441 work at 4.93 s. The `CADI7441_*` path is presumably for a
sibling board; chasing it was a detour.

`accessRegs_viaNUC` reaches the wire through `HAL::setI2C_sw` (`0x176b0`) and
`HAL::getI2C_sw` (`0x175a0`), which call HAL object slots `+0x18` and `+0x20`.

### The I²C transports, resolved

`CI2C_Constructor` (`0xa5310`) installs three slots per type, and the session builds
`type(1)` and `type(6)`:

| slot | role | type 1 | type 6 |
|------|------|--------|--------|
| `+0x38` | read | `CUsbCntl_I2CRead` (op `0x08`) | `CUsbCntl_SWI2CRead` (op `0x0c`) |
| `+0x40` | write | `CUsbCntl_I2CWrite` (op `0x05`) | `CUsbCntl_SWI2CWrite` (op `0x0b`) |
| `+0x48` | write-then-read | `CUsbCntl_I2CWriteThenRead` (op `0x08`) | `CUsbCntl_SWI2CWriteThenRead` (op `0x0c`) |

`CQLCodecLib_Get`'s `QPCODEC_DIAG_I2C_WRITE_THEN_READ` handler calls slot `+0x48`,
which is how the driver polls the MCU at 7-bit `0x15` every ~0.75 s.

## I²C works — on a freshly plugged card

**This is the state that had never been tested.** On a card straight from a replug,
with no firmware downloaded and the ARM left alone, after writing the GPIO defaults
(`0x610 = 0`, `0x614 = 0`, as `CQLCodec_SetGPIODefaults` does just before the input
init), a SW-I²C scan finds responders:

```
slave 0x00 -> status 0x08   ACK
slave 0x15 -> status 0x08   ACK      <- the NUC100 MCU
slave 0x80 -> status 0x08   ACK      (bit-7 alias of 0x00)
slave 0x95 -> status 0x08   ACK      (bit-7 alias of 0x15)
```

Bit 7 of the slave byte is ignored, so the real responders are `0x00` and `0x15` — and
`0x15` is exactly the MCU address the driver polls.

Reading it back confirms the board beyond doubt:

```
$ gl310i2c --sw --no-hold --read 0x15 0x00 24
00 43 36 48 05 0d 0c 10 12 01 47 33 31 30 00 ...
   C  6  H                    G  3  1  0
```

**`G310`** at offset 0x0a, `C6H` at 0x01, and version/date bytes between. So SW-I²C
(op `0x0c` / `0x0b`) is fully functional in loader mode, and the MCU answers.

Why it failed before: every earlier attempt was made either with the main firmware
running — whose gadget implements no I²C at all — or with the ARM halted after a
firmware boot. Neither is the loader state the vendor driver uses.

> Not yet isolated: whether writing the GPIO defaults was actually required, or whether
> the fresh-plug loader state alone is sufficient. The two were changed together.

### The MCU protocol

`InterfaceNUC100::accessRegs_viaNUC` (`0x20b40`) bridges to chips behind the MCU. Its
request struct is `{ slave, dir, reg, _, data }` with `dir` 1 = write, 0 = read, and
the MCU's own address comes from `this[8] >> 1` — i.e. the object stores the 8-bit
`0x2a` and shifts to 7-bit `0x15`, confirming the 7-bit convention throughout.

```
write:  setI2C_sw  0e <slave> <dir> <reg> 01      (5 bytes)   delay 5 ms
        setI2C_sw  13 <data>                      (2 bytes)   delay 5 ms
        setI2C_sw  12 01                          (2 bytes)
read:   setI2C_sw  0e <slave> <dir> <reg> 01 01   (6 bytes)   delay 15 ms
        getI2C_sw  sub 0x13, 1 byte               -> data
```

So MCU command `0x0e` sets up a transaction, `0x13` carries the data byte, `0x12`
triggers it. `HAL::setI2C_sw` takes `{slave, len, bytes...}` and `HAL::getI2C_sw` takes
`{slave, wlen, wdata[8], rlen, rdata[]}` — which map onto ops `0x0b` and `0x0c`.

### Reading the input signal, confirmed on hardware

`getHdmiVideo_6604` (`0x211d0`) does **not** use `accessRegs_viaNUC` — it reads MCU
registers directly, because the MCU already owns the IT6604 and reports what it sees:
`0x1b` (1 byte status), `0x26` (1 byte), and `0x1d` (7 bytes of timing). The timing
bytes are four 12-bit fields, nibble-packed two per three bytes:

```
WTotal = d[0] | ((d[1] & 0x0f) << 8)      WAct = ((d[1] & 0xf0) << 4) | d[2]
HTotal = d[3] | ((d[4] & 0x0f) << 8)      HAct = ((d[4] & 0xf0) << 4) | d[5]
```

Live, with the camera connected (`gl310i2c --hdmi --no-hold`):

```
MCU 0x1b = 0x00   0x26 = 0x03
MCU 0x1d = 98 78 80 33 22 1c 2e
active 1920 x 540, total 2200 x 563
-> 1920 x 1080 interlaced (540 lines per field)
```

2200 x 1125 total, 1920 x 1080 active, 540 lines per field: **textbook 1080i59.94**,
and exactly what the Windows driver logged (`monitor match (WAct x HAct = 1920 x 540)`).

**So the input is healthy and locked right now.** When the encoder produced a
structurally perfect transport stream with no picture in it, the camera was not the
problem — the receiver was seeing the signal all along. Whatever is missing is between
the receiver and the encoder's video input unit.

We have never written a single receiver register, yet it reports a locked 1080i signal.
That is suggestive but **not proof** that the MCU configures the receiver autonomously —
see [§ Still open, stated honestly](#still-open-stated-honestly).

### Proven by static analysis, then tested

The earlier claims in this section were inference. Here is what is actually
established, with the test that settles each.

**1. `CADI7441` uses HW I²C at slave `0x21`.** `CADI7441_Constructor` stores arg6 into
`this[0x3a8]` and arg7 into `this[0x3b0]`; the call site in `CQLCodecLib_CreatePeripherals`
(`0x44628`) passes `lib->[0x208] + 0x38` and the immediate **`0x21`**. `writeRegister`
(`0x9aa30`) calls slot `+8` of `this[0x3a8]`, i.e. `CI2C + 0x38 + 8 = CI2C + 0x40`, the
write slot. `lib->[0x208]` is the **first** CI2C constructed (`0x44db3`), and the log's
first `CI2C_Constructor() type(%d)` line is `type(1)` — whose slots resolve to
`CUsbCntl_I2C*`, ops `0x08`/`0x05`. So: HW I²C, slave `0x21`. Not `0x31`/`0x35` — those
are separate literals used only inside `CADI7441_InitDevice`.

**2. Op `0x08` is implemented by the device; it is the transaction that fails.**
Tested by comparing against a deliberately invalid opcode:

```
op 0x7f (invalid)  -> LIBUSB_ERROR_TIMEOUT, no reply at all
op 0x08 (HW I2C)   -> replies 00 00   (status 0x00)
op 0x0c (SW I2C)   -> replies 00 08   (status 0x08)
```

An unimplemented opcode produces no reply. Op `0x08` replies, so it is dispatched and
the I²C transaction itself is failing.

**3. SW I²C status codes: `0x08` = ACK, `0x06` = NAK.** Tested on a card where the
MCU's presence is independently known: `0x15` returns `0x08`, `0x21` returns `0x06`.

**4. SW I²C works with the firmware running too.** Previously claimed otherwise. Tested
back to back in one session: loader mode `0x15` → `0x08`; after `gl310init --go`,
`0x15` → `0x08` again. The earlier `0x06`-for-everything observation came from a card
already degraded by a capture run. **The firmware string
`QPSOSI2CTransfer: Software (GPIO) I2C not supported` describes QPSOS's *internal*
i2c.c, not the host command path**, and must not be read as "op 0x0c is unsupported".

**5. Error-level logging was enabled in the Windows capture**, so an absent error
message is meaningful. Extracted all 828 level-2 format strings from the driver and
grepped the log: 18 appear, including `CQLCodec_InitDevice() config to use external
Audio FW`. Since neither `CADI7441_WriteBlock() ... writeRegister failed` nor
`CUsbCntl_I2CWrite() Failed I2C status` appears, **the ADV7441 writes genuinely
succeeded on Windows.** That retracts the guess that the `CADI7441` path is vestigial.

**6. `QPHCI_Init` is not the gate.** Counting *all* calls to the register-write slot
rather than only immediate-numbered ones: `QPHCI_Init` makes **5** register writes,
`QPHCI_ReInit` makes **5**, and neither reads any. Its extra bulk is thread creation and
`QPHCI_PowerUp`, which is a proven no-op when field `0x3c8 == 8`.

**7. The HCI windows come up already configured** on a freshly plugged card
(`0x81c = 0x4000`, `0x820 = 0x103fff`, ...), so the loader programs them itself.
Re-applying the `QPHCI_ReInit` sequence changed nothing and did not revive HW I²C.

**8. `0x50` bit 8 (pad power-down) is already clear** on a fresh card (`0x50 = 0x404`),
so pad power is not gating I²C either.

### HW I²C is fire-and-forget — which invalidates the Windows evidence

Tested with controls, and this overturns the conclusion above.

```
SW write  0x15 (present) -> 0x08      SW write 0x21 (absent) -> 0x06
SW write  0x4e (absent)  -> 0x06      SW read  0x21          -> 0x06
HW write  0x31 -> 0x08   HW write 0x21 -> 0x08   HW write 0x4e -> 0x08
HW read   (any address)  -> 0x00
```

SW write status is meaningful: it reports NAK. **HW write status is not** — it returns
`0x08` for every address, including ones with nothing on them. HW reads always fail,
including write-then-read with a sub-address.

So **retract claim 5 above.** The driver only ever *writes* to the ADV7441
(`InitDevice`, `SelectVideoSource`, `SetVideoRes` are all writes), and
`CUsbCntl_I2CWrite` logs a NAK only when the status byte is not 8 — which on this
transport it always is. The Windows log would therefore show no I²C error **whether or
not the chip was there or the master worked**. Its silence proves nothing.

Also worth recording: `CUsbCntl_I2CWrite` *logs* a bad I²C status but returns the
GenericCmd status, so a NAK never propagates to callers. `CADI7441_WriteBlock`'s error
message could not fire on a NAK even in principle.

### Replaying the receiver init does change the stream

With the correct 7-bit addressing (the driver passes `0x31`/`0x35`/`0x21` straight into
the slave byte — `--shift 0`), in loader mode, before the firmware download:

```
gl310i2c --init --shift 0 --go     # 8 writes to 0x31/0x35, then 22 to 0x21
gl310init --go
gl310start --go
```

The captured elementary stream changes substantially:

| | before receiver init | after |
|---|---|---|
| entropy | 5.396 bits/byte | **6.914** |
| `0x00` share | 27 % | 0.7 % |
| `0xff` share | 14 % | 3.5 % |
| TS continuity | 100 % | 28315/28316 |

And the PES layer is now unambiguously valid — headers are `00 00 01 e0` with
stream_id `0xe0`, PTS present, and **PTS increments by exactly 3003 ticks at 90 kHz =
29.97 fps**.

> One correction to my own reasoning here: I initially called the drop of `00 00 00`
> from 448,928 to **0** strong evidence of H.264 emulation prevention. It is not. At
> `P(0x00) = 0.0068` over 5.2 MB the expected count by chance is about **1.6**, so zero
> is unremarkable. The entropy and byte-distribution changes are real; that one was not.

**It is still not decodable video.** There is no SPS anywhere in the stream (0 NAL
type 7, against 24 PPS and 1 IDR), so `ffmpeg` reports `dimensions not set` and decodes
0 frames. Only 5 PES starts appear in 28,317 packets, i.e. about 1 MB per frame where
1080i at 8 Mbps should be ~33 KB — so the capture still contains far more data than the
encoder can be producing, even though TS continuity says the packet sequence is
contiguous. Those two facts are not yet reconciled.

### Still open, stated honestly

HW I²C (op `0x08`) returns status `0x00` for **every** address in **every** state
tested — loader mode, firmware running, before and after HCI setup, with and without
GPIO defaults. Yet the same transport demonstrably worked on Windows. Nothing found so
far accounts for the difference. Candidates not yet eliminated: the Windows card had a
*previous driver session* still resident (its teardown is at the head of the log), so
the ARM was running that session's firmware at 4.93 s rather than a loader; and the HW
I²C master may need configuration done somewhere not yet located.

Consequently **it is not established** whether the MCU configures the receiver
autonomously. What *is* established is that the receiver reports a locked 1080i signal
without us having written any receiver register.

### The bridge works, but cannot be used to probe

`gl310i2c --nuc-read SLAVE REG` implements `accessRegs_viaNUC` and returns status
`0x08`. But a sweep of all 128 slave addresses returns `0x00` for every one of them, so
the MCU reports zero rather than an error when nothing answers, and **presence cannot
be probed this way**. `--nuc-scan` is kept but its output means nothing on its own.

### `CADI7441_SelectVideoSource`, extracted but probably moot

`CADI7441_WriteBlock` (`0x9b940`) walks an array of `u32` pairs — register, value —
terminated by `-1`, writing each with `writeRegister` (`0x9aa30`), which sends
`{reg, val}` to the slave in `this[0x3b0]` through the object at `this[0x3a8]`, calling
its slot `+8`. That object is *not* a `CI2C` instance (those install at `+0x38`/`+0x40`/
`+0x48`), and has not been identified.

`CADI7441_SelectVideoSource(source)` picks one of four tables — sources 1, 10, **20**
and 40. Source 20 is what the log shows, and its table is:

```
00=00 03=09 04=47 05=06 06=02 17=01 1d=40 31=12 34=00 35=02 37=00
3a=01 3c=58 47=00 68=f0 69=00 6b=e3 7b=0d ba=a0 c8=08 f3=00 f4=3f
```

`CADI7441_SetVideoRes` similarly writes tables for 640x480, 720x480, 720x576, 1280x720
and 1920x1080 at 25 or 50 fps.

**That guess was wrong.** Error-level logging was enabled in the Windows capture and no
I²C failure was recorded, so these writes succeeded there. The `CADI7441` path is real
on this board; it is our HW I²C that does not work. The register tables are therefore
worth keeping, and the open question is why op `0x08` fails here.

### The step we still skip

`CQLCodecLib_InitDevice` (`0x3ffd0`) is the outer bring-up: it calls `QPHCI_ReInit`,
`CQLCodec_InitializeMemory`, `CQLCodec_AOSwitch`, `CQLCodec_SetGPIODefaults`,
`CQLCodec_FWDownloadAll` **and `CQLCodecLib_InitPeripherals`**. `gl310init` replicates
only the `FWDownloadAll` half. The peripherals half is where the receiver setup lives.

### `CQLCodec_FWSwitchMode`, and why not to run it

The driver can move the ARM between loader and main firmware — the gadget's
`RESET_ARM` handler prints `be in loader` / `be in main` / `still in loaderFw` /
`still in mainFw`. `CQLCodec_FWSwitchMode` (`0x58eb0`) is the mechanism. It reads the
`QSOS` header at DRAM `0x100`, takes the version from the halfword at `0x102`, picks an
entry base (`0x2f2000` for QPSOS2, `0xf2000` for QPSOS3+), then:

```
MemoryWrite(0x2f1090, 1)
MemoryWrite(0x2f2004, 1)     /* base + 4 */
ResetArm(0)
DelayMilliS(1)
RegisterWrite(0x6cc, 0)
ResetArm(1)
```

> **Do not run this without being able to replug the card.** Tried once: the device
> disappeared from USB entirely — not a stalled gadget, not enumerated at all, absent
> from `ioreg`. Only a physical replug brought it back. The mode switch tears down the
> gadget and whatever it lands in never re-advertises itself. A replug does clear DRAM,
> so the flags do not persist and the card returns healthy, but `--unswitch` is useless
> because by then the device is unreachable.

So reaching the loader is the open problem. Options not yet explored: whether the chip
runs the loader *before* any firmware download (i.e. do the I²C init on a freshly
plugged card, which is exactly the vendor's ordering, and is the one state never
tested), and what the loader's own USB identity is.

GPIO `0x618` reads `0x0000ff1f`, so bit 12 is set, matching
`QPCODEC_GPIO_BIT_VALUE bit(12) val(1)` in the working session. `AVer_GPIOI2C` turned
out to be the AT88 crypto chip, not the video path.

### What the faster loop revealed

Batching the read side and throttling the idle poll changed the picture, and not
entirely in the direction expected. Three runs, same 3 s window:

| idle poll | polls/s | fragments | bytes | implied rate |
|-----------|---------|-----------|-------|--------------|
| 8 ms sleep, unbatched | ~120 | 55 | 3.1 MB | 8.2 Mbps |
| none (flat out) | 5381 | **0** | 0 | — |
| 1 ms sleep, batched | ~366 | 455 | 27.1 MB | **72 Mbps** |

Two things follow.

**Polling flat out starves the firmware.** At 5381 polls/s the card produced *nothing*
at all — the HCI thread never got a look in. There is a throttle requirement here, not
just a round-trip budget.

**The captured volume scales with how hard we poll, so the ring is being re-read.** In
the 455-fragment run the descriptor `(p2 = 0x102f00, p4 = 0x5d00)` — base address, full
ring size — appears **115 times**, while every other descriptor appears exactly once.
The remaining 171 `p3=0`/`p3=1` pairs each sum to the whole ring again, so even after
discarding the repeats the implied rate is ~43 Mbps against a configured 8 Mbps peak.

The honest reading: **`cmd 0x40` reports ring *state*, not a queue of new data**, and the
host is expected to tell the ARM how much it consumed. That is what
`CTask_CompleteArm`'s parameters do, and blindly echoing the incoming `p1..p5` with a
status of 1 is evidently not it — `CTask_CompleteArm` sources its six parameters from
driver-side request bookkeeping (the log shows `status(0x1) reqid(951)`), and
`CEncoderTask_ProcessArmMessage` tracks `dataType`, `#(n)` and `msg_id` per task.

So the next step is decoding that bookkeeping properly, rather than tuning the poll
loop. Until then the firmware reports its unhappiness plainly: every run ends with a
few hundred `(E)Drop` lines and `(E)CODEC_ERR = c0009` / `= 9800b`, followed by
`(T)ARC Rec Stop`.

### `RegisterWriteEx` (op `0x03`) does not do what the driver implies

`CUsbCntl_RegisterWriteEx` (`0x83f50`) builds `03 01 n:u16 reg:u32` followed by `n`
values at offset 8, total length `8 + 4n` — and `RegisterReadEx` with the mirror layout
is verified 1:1 against the known mailbox block. But writing five values from `0x6d0`
on the live card produced:

```
0x6d0 <- value[1]    0x6d4 <- value[2]    0x6d8 <- value[3]
value[0] and value[4] landed nowhere visible
```

Until that is understood, **batch reads but not writes.** `gl310start` uses six single
`RegisterWrite`s for the ack parameters, which keeps the per-fragment cost at twelve
transfers rather than nine — still well under the original thirty.

### Operational rule: reboot the firmware after every capture run

A capture run leaves the gadget degraded even when it still answers — `gl310log` fills
with `(E)Drop` and `can't get sysmsg` — and **the next heavy operation then wedges it
for good.** The third wedge was exactly that: `gl310init --go`'s firmware download run
against an already poisoned gadget, which hung mid-download.

`ResetArm` is two OUT-only commands and gives QPSOS a clean gadget in 8 ms, so
`gl310start` now always does it on the way out. Re-download the firmware with
`gl310init --go` before the next capture.

Note also that `swap` is not host-side software byte swapping: `CQLCodec_StartDMARead`
turns `swap != 0` into a mode value of 3 passed down to the DMA engine, and
`CUsbCntl_StartDMARead` has no swap field in its 16-byte command — so it is a register
the HCI layer programs. Swapping in software works fine meanwhile.

### Hazard: the command channel can wedge

`CUsbCntl_GenericCmd` is a bare request/response pair on two bulk pipes with no
framing and no sequence numbers, so it cannot resynchronise. Once the encoder is
running and also pushing data on the DMA-in pipe, an abandoned or unread reply leaves
every later read timing out. `tools/gl310recover.c` escalates: drain the IN pipes,
`clear_halt`, then a `ResetArm` toggle — which works even with the IN path dead,
because `ResetArm` is OUT-only.

**Do not use `libusb_reset_device()` on macOS for this.** It left the card enumerating
perfectly (`ioreg` shows `Aver_C835_USB`, `07ca:c835`, registered/matched/active)
while `libusb_open()` returned `LIBUSB_ERROR_OTHER` and then hung outright. Nothing in
user space recovered it — it needed a physical replug, which also costs the warm
state. That reset is now last and behind `--hard` in `gl310recover`.

## Still to do

The protocol is finished. What remains is engineering, and one piece of it is now
clearly on the critical path.

- **Transport is done.** The consumed-count ack gives 100% TS continuity with zero
  loss, so asynchronous I/O is no longer on the critical path - it is a latency and
  CPU optimisation for later.
- **Decode `InterfaceNUC100::accessRegs_viaNUC` (`0x20b40`).** SW-I²C to the MCU at
  `0x15` now works on a freshly plugged card, so the transport is solved; what remains
  is the MCU's own command format for reaching IT6604 registers behind it. Then
  `setupHdmiVideo_ex` to configure the input, and `getHdmiVideo_6604` to confirm the
  1920x540 signal the Windows driver saw.
- **Order of operations for a capture run** is now: replug or cold card → GPIO
  defaults → MCU/IT6604 setup over SW-I²C → `gl310init --go` → `gl310start --go`.
- **Task lifecycle.** A second `StartEncoder` after a `StopEncoder` produces no frames;
  the inbound mailbox then shows `cmd 0x50` (encoder stopped). Either send
  `SystemClose` (`0xf3`) first or just reboot the firmware between runs, which takes
  8 ms and is what `gl310init --go` already does.
- Then the portable `libgl310` core, a macOS CMIOExtension and a Linux v4l2loopback
  sink, decoding the TS with VideoToolbox / VAAPI.

### Cold-start recipe, as it now stands

```
gl310init  --go        # 9-step bring-up + firmware download; QPSOS boots in ~8 ms
gl310log               # confirm 46 lines ending "Start Update Tick Thread"
gl310start --go        # configure + StartEncoder + capture, writes .bin and .bin.idx
```

Reassemble by concatenating every fragment from the `.idx` in order and swapping each
32-bit word. DDR training is **not** required: the `0xf00` block comes up already
holding every value the vendor writes, and a 48 MB write/read-back test passes before
anything is touched (`gl310ddr --enable-only --go` demonstrates this; `0xf18` is
read-only and ignores writes).
- **Post-boot init.** `CQLCodecLib_InitDevice`, then the `CQLCodec_Set` properties
  for stream type/profile/level, then encoder start (`CEncoderTask_*`,
  `QPFWENCAPI_*`). Every one of these should now be visible in `gl310log --follow`.
- **Check the HDMI front end.** `CADI7441_InitDevice` for the ADV7441, and the
  SW-I²C poll of slave `0x2a` sub `0x1b` that reports input status. The `0x06`
  status from op `0x0C` may simply have been the loader's I²C, which the booted
  firmware replaces.
- Then the portable `libgl310` core, a macOS CMIOExtension and a Linux
  v4l2loopback sink.

### Instruments

| tool | what it does | safety |
|------|--------------|--------|
| `gl310log` | reads/follows the firmware's own log | read-only |
| `gl310life` | wide DRAM diff; proves execution and locates live structures | read-only |
| `gl310probe` | registers, memory dumps, raw commands | read-only unless `--allow-write` |
| `gl310aperture` | measures the HCI window translation | writes 3 regs, restores them |
| `gl310init` | the 9-step bring-up and firmware download | writes DRAM |
| `gl310armtest` | bare ARM stub loader | **destroys the image header at `0x100`** |
| `sysmap.py` | name/disassemble the driver by its debug strings | static |
| `fwarm.py` | ARM32 firmware analysis | static |

## Prior art: sibling devices and what they tell us

Searched for other AVerMedia reverse-engineering work, because this hardware family is
clearly shared and someone else may have solved parts of it.

### `FireCulex/avermedia-c985-linux` — the closest relative by far

A Linux V4L2 driver for the **AVerMedia C985 (Live Gamer HD)**, described as
implementing the card's *"vendor-specific mailbox-based firmware protocol,
reverse-engineered from the Windows driver and ARM firmware"*. The correspondence with
this card is extremely close:

| C985 (PCIe) | GL310 / C835 (USB) |
|-------------|--------------------|
| Nuvoton **NUC100**RD2BN MCU | Nuvoton NUC100, 7-bit `0x15` |
| `qpvidfwpcie.bin` + `qpaudfw.bin` | `qpvidfwusb.bin` + `qpaudfwusb.bin` |
| mailbox protocol, debugfs `mbox_log` | mailbox at `0x6b0`/`0x6cc`/`0x6fc` |
| `c985_nuc100.c` MCU register access | `InterfaceNUC100::accessRegs_viaNUC` |
| `cpr.c` CPR register helpers | `CPR_MemoryRead` / `CPR_MemoryWrite` in our driver |
| firmware-managed **4-slot ring buffer** | the ring we read via `cmd 0x40` |

So the same vendor stack spans PCIe and USB variants, which is why our driver binary
carries `CPCIeCntl_*` alongside `CUsbCntl_*`. The two families differ only in transport.

**The interesting divergence: the C985 driver outputs YUV420 (YU12) 1920x1080, not
H.264.** Its README mentions no SPS/PPS or H.264 parsing at all.

### This card has a raw path too

Confirmed locally, in our own binary and log:

```
CTaskRawVideo::CTaskRawVideo / getOutputResolution / setOutputResolution
CDevice::allocateRawVideoOuputTask / releaseRawVideoOuputTask
%s(): ARM_BUF_YUV / ARM_BUF_YUVMB2RAS / ARM_BUF_YUVRAS / ARM_BUF_OTHERS
CTask_BuildIoBlockYUV / CTask_BuildIoBlockYUVMB2RAS / CTask_BuildIoBlockYUVRAS
```

and from the Windows session:

```
CDevice::Init release the raw tasks (m_dwDisableRawOutput = 1)
```

**Raw video output exists on this hardware and the Windows driver switched it off via a
registry value.** Every buffer we have captured was `ARM_BUF_OTHERS` (the compressed
path); `ARM_BUF_YUV*` are dispatched on `[rsp+0x7c]` in
`CEncoderTask_ProcessArmMessage`, i.e. selected by how the task is configured.

Raw YUV would be strictly better for a webcam: no SPS/PPS problem, no decoder, lower
latency. It is now the most promising direction.

### `SystemControl` decoded — and the stream type is fixed at TS

The log prints `QPCODEC_PROP_SYSTEM_CONTROL stream type(1) stream data(3) profile(2)
level(12) ff_mod(1)`, and `CQLCodec_Set`'s packing of that property gives register
`0x6f8`:

| bits | field | value in `0x2101c219` |
|------|-------|----------------------|
| 0-2 | stream type | **1** |
| 3-7 | stream data | 3 |
| 8-11 | profile | 2 |
| 12-15 | level | 12 |

which reproduces the logged values exactly, so the decode is confirmed.

**Tested: stream type is not the lever.** Sweeping bits 0-2 with everything else held
constant, rebooting the firmware between runs:

| stream type | result |
|-------------|--------|
| 0 | 0 fragments |
| 1 | 507 fragments in 2 s (control, works) |
| 2 | 0 fragments |
| 3 | 0 fragments |
| 4 | 0 fragments |

Only TS produces output in this configuration. Selecting the raw path must therefore
happen elsewhere — the task/channel `dataType`, or `SYS_FUNCTION` (`0x80000011`), or
`SYS_LINK`'s `video_output` field — not in `SystemControl`.

### The YUV selector found: it is `p1`, not any config field

`CEncoderTask_ProcessArmMessage` dispatches through a jump table on **`p1`** — the
stream type in the ARM's message — not on the command code. Extracting the table
(index bytes at RVA `0x79e44`, targets at `0x79e30`, entries are RVAs):

| `p1` | buffer type |
|------|-------------|
| `0x00` | `ARM_BUF_YUVRAS` |
| **`0x80`** | **`ARM_BUF_YUV` / `ARM_BUF_YUVMB2RAS`** |
| `0x01` `0x06` `0x81` `0x82` `0x83` `0x84` `0x85` `0x86` | `ARM_BUF_OTHERS` (compressed) |
| everything else | unhandled |

Every buffer we have ever captured has `p1 = 0x83`. **Raw YUV is `p1 = 0x80`**, so the
question is only what makes the ARM tag its buffers that way.

### Four candidate levers, all swept, none of them it

Each run: reboot the firmware, apply the change, capture 2 s, read `p1` of the first
buffer.

| lever | values tried | result |
|-------|--------------|--------|
| `SystemControl` stream type (`0x6f8` bits 0-2) | 0, 2, 3, 4 | **0 fragments**; only 1 streams |
| `SystemLink` `video_output` | 0, 2, 3 | 0 and 2 dead; 3 streams but `p1 = 0x83` |
| `SetEncMode` capMode (cmd `0x11`) | 0, 1, 2, 3 | 1-3 dead; 0 streams, `p1 = 0x83` |
| `SetRawVideoDecimation` output_format (sel `0x11`) | 1, 2, 3 | all stream, all `p1 = 0x83` |

The pattern is informative: most deviations stop the encoder dead, while
`SetRawVideoDecimation` is accepted without disturbing it and changes nothing. That is
what a *configuration* for a path that is not running looks like.

So raw output is very likely **a separate ARM task**, not a mode on the encoder task —
matching the host side, where `CDevice::allocateRawVideoOuputTask` creates a distinct
`CTaskRawVideo` object with its own `setOutputResolution` / `setOutputFrameRate` /
`setDeInterlace`, alongside the encode task.

**Next test:** every mailbox message we send uses `taskId 0`. Open a *second* task —
`SystemOpen` / `SystemLink` with `taskId 1` — and start it, then watch for buffers
tagged `p1 = 0x80`. `CYUVInChannel_GetBuffer` / `CYUVInChannel_CompleteBuffer` are the
host-side counterparts to the `cmd 0x40` / `cmd 0x30` pair we already drive for the
compressed task.

## What was actually wrong with the H.264 — it is one missing piece

Took the reassembly logic out of the loop entirely (`gl310start --ring-dump ADDR LEN`
reads one contiguous span of the encoder's ring with no notifications, acks or
fragment joining) and analysed a single internally-consistent fragment.

**The stream is structurally valid.** From one 95,128-byte fragment:

```
TS sync            505/505 packets
PID 0x44 payload   92,728 bytes, 1 PES start
payload head       00 00 01 e0 | 00 00 | 80 c0 0a | PTS | DTS      <- valid PES header
                   00 00 00 01 09 10                               <- valid AUD NAL
                   3f f7 26 6e ...                                 <- slice data
```

Across 528 fragments in 2 s: **113 AUDs** (≈ one per field, exactly right for
1080i59.94), 186 non-IDR slices, 1 IDR, 4 SEI. PTS/DTS present and incrementing by
3003 ticks at 90 kHz = 29.97 fps.

**The only thing missing is the parameter sets.** Scanning every fragment of a 2 s and
then an 8 s capture: **SPS count 0.** Without an SPS a decoder cannot learn the
resolution or profile, so `ffmpeg` reports `dimensions not set` and decodes nothing —
not because the data is bad, but because it is missing the key to read it.

### Correcting my own earlier diagnosis

My "the payload is garbled" conclusion was substantially an analysis error:

- **A slice NAL is one long run with no internal start codes** — emulation prevention
  guarantees it. So 92 KB with no start codes after an AUD is *correct structure*, not
  corruption. I read normal H.264 as damage.
- My NAL scans ran over *concatenated* fragments and mid-slice data, so they found
  `00 00 01` by chance and reported nonsense NAL types (2, 3, 4, 16, 24 …). Those were
  artefacts of the scan, not of the stream.

The one real signal in the earlier data was the pre-receiver-init capture being 27 %
zero bytes with entropy 5.4 — that genuinely was an empty stream, and the receiver init
genuinely fixed it.

### Encryption is ruled out

`transport_scrambling_control` is `0` on every packet, and the PES flags byte `0x80`
has `PES_scrambling_control = 00`. Both layers declare the content unscrambled, and the
content decodes as valid PES/NAL structure, which encrypted data would not.

### Where the parameter sets have to come from

Not the firmware (`qpvidfwusb.bin` contains no Annex-B parameter sets) and not the
driver (the two `00 00 00 01 67` hits in `AVer330USB.sys` are false positives inside a
lookup table — the surrounding bytes are a regular `00 00 00 NN 67 00 00 XX` pattern).
No `QPFWENCAPI_*` property for repeating or inserting headers was found.

So the encoder appears simply never to emit them, and the Windows application supplies
them from its own configuration. Options, cheapest first:

1. **Record once on the Windows machine** with the vendor software and lift the exact
   SPS/PPS out of the resulting file. They depend only on the encoder configuration,
   which we reproduce byte for byte, so one capture serves forever.
2. Synthesise them from what we know (1920x1080, Main profile, level 4.0, interlaced,
   29.97 fps). Workable but must match the slice headers exactly.
3. Keep looking for a "repeat sequence header" property.

With the parameter sets prepended, this stream should decode as-is.

## What the vendor applications tell us

`vendor/LGPLite_Stream_Engine_V1.3.0.16_*.exe` (NSIS, extract with `7z x`) and
`vendor/RECentral_1.3.0.121.zip` (loose DLLs, not buried in the MSIs).

### Stream Engine uses the H.264/TS path, not the raw path

This matters because it validates the whole approach. The GL310-specific components are

```
Filter/AVerMedia LGPLite/avmC835devicecontrol_X64.dll
Filter/AVerMedia LGPLite/avmC835virtualvideocapture_X64.ax
Filter/AVerMedia LGPLite/avmC835virtualaudiocapture_X64.ax
```

and the device-control DLL contains the strings **`C:\AVerDump.ts`** and
`..\AVerDump.ts`, plus classes `CAVerCapDeviceC835` / `C875` / `C985` and the build
path `D:\Working_space\AP Team\StreamEngine_V1\VirtualFilter\C835VirtualCapture\...`.
A `.ts` dump path means it consumes a transport stream — the same thing we capture.

Searching every Stream Engine binary for `RawOutput` / `RawVideo` / `DisableRawOutput`
finds **nothing**. So the webcam path is *not* the raw YUV path; it decodes H.264.
`avmC835virtualvideocapture_X64.ax` only advertises YUY2 and RGB variants and contains
no H.264 code, so the decode happens upstream (`Filter/X64/avmdcm.dll`, 5.4 MB, whose
strings identify no third-party codec).

RECentral ships `Components/DeviceAPI/C875Device.dll` and no `C835Device.dll` — matching
the `ProjectC875` seen throughout our driver log, so the GL310 is handled by the C875
project there.

**Conclusion: decoding the H.264 TS is what the vendor's own webcam does.** Chasing the
raw path is optional, not required.

### Still no parameter sets

- The **PMT carries no descriptors at all** (`program_info_length = 0`,
  `ES_info_length = 0` for both streams), so there are no out-of-band parameter sets in
  the transport stream.
- Neither vendor binary contains a plausible pre-built SPS. The `00 00 00 01 67` hits in
  `AVer330USB.sys` are inside a lookup table; the two-byte `67 4d` style hits in the
  Stream Engine binaries are at chance frequency for 15 MB of compressed data.
- A capture at a deliberately lowered `VBRBitRate` found one `00 00 01 27` candidate,
  but its `profile_idc` is `0x03`, which is not a valid H.264 profile — a false
  positive inside slice data.
- **`VBRBitRate` (register `0x6e8`) does not change the output rate.** Setting it to
  500/200 kbps still produced 15.5 Mbps, so effective rate control lives elsewhere.
  That also kills the idea of slowing the ring fill to catch the stream opening.

### The cheap way to settle it

`C:\AVerDump.ts` is a debug dump built into the Stream Engine's device-control DLL. If
it can be triggered on the Windows machine, it gives **ground truth**: either the
driver's transport stream contains SPS/PPS and our configuration differs from the
vendor's, or it does not and the decoder is given the parameters some other way.

Failing that, recording a few seconds in RECentral and extracting the SPS/PPS from the
resulting file works just as well — they depend only on the encoder configuration,
which we reproduce byte for byte.

## The real defect: 60 Mbps into a 128 KiB bitstream buffer

> **Update 2026-10-06.** The P-frames we capture are valid H.264. They decode with zero
> errors once given the right parameter sets. What breaks the stream is the configured
> bitrate.

### Correction: RECentral does not use our configuration

The last section assumed RECentral's SPS/PPS would fit our stream because "we reproduce
the configuration byte for byte". That assumption was wrong. RECentral's recording is
CABAC (`entropy_coding_mode_flag = 1`) at about 12 Mbps. Our configuration, copied from the
driver session in `gl310-bringup-debugview.log`, says:

```
QPCODEC_PROP_RATE_CONTROL bitrate(60000) qp_update(120) mode(0) fixed(0) vbr(0)
QPCODEC_PROP_SYSTEM_CONTROL stream type(1) stream data(3) profile(2) level(12) ff_mod(1)
    spsr_freq(1) v_mode(0) cabac_init(0) ver(1) xfer_mode(0)
```

That is **CBR 60 Mbps, CAVLC**. `RateControl` = `0x0078ea60`: the low 16 bits are the
bitrate in kbps (0xea60 = 60000), and bits 16+ are qp_update (0x78 = 120). `VBRBitRate`
(`0x6e8`) is unused when vbr = 0, which is why changing it never moved the output rate.

### Our P-slices decode cleanly

The slice headers of our PES 1–3 (`41 e0 08 10 04 31 83 07 f0…`) show frame_num 1, 2, 3
and POC 2, 4, 6. That matches the vendor SPS layout: 10-bit frame_num, 8-bit POC lsb. Read
with the vendor PPS, the next field is `cabac_init_idc = 31`, which is invalid.

Each candidate PPS was rebuilt and fed to ffmpeg together with our three P-frames
(scratchpad `ppstest.py`), keeping the vendor SPS:

| entropy | deblock ctrl | transform_8x8 | ffmpeg errors |
|---|---|---|---|
| CABAC | any | any | 16 (`cabac_init_idc 31 overflow`) |
| CAVLC | 0 | any | 6 |
| CAVLC | 1 | absent / 0 | 8 (`P sub_mb_type 32 out of range`) |
| **CAVLC** | **1** | **1** | **0** |

Three full frames, 3 × 8160 macroblocks, parse with no errors. Random data parsed as
CAVLC fails within a few macroblocks. Rendered over a gray reference, the frames show
recognisable picture content: a face outline and the pillarbox edges.

### Why the IDR and some P-frames are garbage

In each capture's first PES (the IDR, about 195 KB), the bytes at offset *k* reappear at
*k* + 131072. The same holds for every 4 KiB probe from 6 to 61446, in both `full.bin`
and `delay20.bin`. The encoder writes into a **128 KiB circular bitstream buffer**, and a
frame larger than that overwrites its own beginning: AUD, then SPS, PPS and the slice
header. The data fits this exactly:

| capture | PES | size | result |
|---|---|---|---|
| full.bin | 0 (IDR) | 195610 | head overwritten |
| full.bin | 1–3 (P) | 95162–127550 | clean `[AUD, slice]` |
| full.bin | 4 (P) | 151754 | head overwritten, wraps at 131078 |

At 60 Mbps and 30 fps a frame averages about 250 KB, so nearly every frame overflows.
**The fix is to lower RateControl's bitrate**: `gl310start --rate KBPS`.

### Firmware: how frame slots are released

- Host cmd `0x30` reaches the ARM as internal message `0xf7`, built by the HCI handler at
  `0x2d5d8`. Its fields are:
  - `[8]` = task (`0x6fc >> 16`)
  - `[0x10]` = `0x6f8 & 0x8f`
  - `[0x14]` = `0x6e4 & 0xff`
  - `[0x1c]` = `0x6f4`
  - `[0x20]` = `0x6ec ? 0x6f0 : 0x80000000`
  - `[0x24]` = per-task queue
- The encoder's `0xf7` case (`0x3c790`) frees a frame slot (`0x3c7bc`, the only store
  that clears slot state) when `[0x20] == task | 0xffff0000`. The drop check is at
  `0x3e4c0`. The same tag is forwarded by another task at `0x1fcb0` from a per-buffer
  field (`+0x558`), so the release normally comes from inside the firmware, not from
  the host.
- Test: `--ack-mode 2` (plain ack, then the release form, for every frame) raised
  throughput to 24 Mbps but destroyed the content: 1 PES start in 14 MB, 260
  continuity breaks. The card then wedged hard (OUT pipe dead; needs a replug).
  **Do not send the release form from the host.** The `(E)Drop`s are better explained
  as a consequence of 60 Mbps frames, and should be re-checked at a sane bitrate.
