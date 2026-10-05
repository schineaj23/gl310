# GL310 (07ca:c835) USB command protocol, recovered from AVer330USB.sys

Source: static disassembly of `vendor/AVer330USB.sys` (3.2802.64.40, the checked build),
using the `CUsbCntl_*` functions located through their own debug strings. The DMA and
mailbox behaviour was cross-checked against `captures/gl310-bringup-debugview.log`
(take 2, a clean restart). RVAs are given so every claim can be re-checked.

USBPcap can't see this device's URBs on Windows (see `captures/NOTES.md`), so nothing
here has been confirmed on the wire yet. The layouts below come from the code; they are
not guesses from traffic.

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

## Still to do

- Register addresses for the mailbox (REG_TO_ARM_MESSAGE*, ARM->host message and status).
- The register sequence in QPHCI_ReInit and InitializeMemory (DDR bring-up).
- The meaning of the 1-byte reply to the DMA command (status? ready?).
- The encoder start/stop message codes (`CEncoderTask_*` / `QPFWENCAPI_*`).
- Confirm on hardware: `GetUSBSpeed` (`0x14`) and `ReadHciRegister` (`0x00`) are harmless
  reads. They should answer even before any firmware is loaded, because the driver issues
  HCI accesses before the download. That makes them a good first libusb test on the Mac.
