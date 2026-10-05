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

## Still to do

- The register sequence in `QPHCI_ReInit` (`0x4aca0`, 811 B) and
  `CQLCodec_InitializeMemory` (`0x4b930`, 2306 B) — **DDR bring-up, the gating
  unknown.** Nothing else can proceed until firmware can be written to DRAM.
  Both are now directly readable: `python3 tools/sysmap.py show QPHCI_ReInit`.
- Why `ReadHciRegister` returns `0xff` — presumably HCI is dark until `QPHCI_ReInit`.
- Why `0x0C` SW-I²C returns status `0x06`; decode `QPPFMGetAttr` in the firmware.
- The meaning of the 1-byte reply to the DMA command (status? ready?).
- The encoder start/stop message codes (`CEncoderTask_*` / `QPFWENCAPI_*`).
- The ARM→host message/status registers (we have the host→ARM side).
