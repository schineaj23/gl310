# qpvidfwusb.bin — the GL310's ARM firmware ("QPSOS")

First-pass analysis of the 454,064-byte ARM32 image shipped in the Windows driver
package. Tool: `tools/fwarm.py` (capstone). Everything below is reproducible with
the subcommands shown.

## Image layout

Flat ARM32 little-endian, load base `0x00000000`, confirmed by a well-formed
exception vector table at offset 0 (`fwarm.py vectors`):

```
 0x00  e59ff018  reset           ldr pc,[pc,#0x18]  -> [0x0020] = 0x0005fbdc
 0x04  e59ff018  undefined                          -> [0x0024] = 0x0005fb90
 0x08  e59ff018  SWI                                -> [0x0028] = 0x0005fba4
 0x0c  e59ff018  prefetch_abort                     -> [0x002c] = 0x0005fb7c
 0x10  e59ff018  data_abort                         -> [0x0030] = 0x0005fb68
 0x14  eafffffe  reserved        b .   (branch to self, as expected)
 0x18  e59ff014  IRQ                                -> [0x0034] = 0x0005fb58
 0x1c  e59ff014  FIQ                                -> [0x0038] = 0x0005fb48
```

All seven handlers live at `0x5fb48`–`0x5fbdc`, inside the image (which ends at
`0x6edb0`). Linear disassembly recovers **111,541 ARM instructions** and 1,386
distinct PC-relative literal constants.

`fwarm.py pools` buckets those constants into candidate MMIO windows:

| window | refs | likely |
|---|---|---|
| `0x00000000` | 2958 | code/data (the image itself) |
| `0x80000000` | 212 | **main peripheral window** |
| `0x00100000` | 67 | HCI/register page (matches the driver's `regBase 0x100000`) |
| `0x00200000` | 63 | |
| `0x7f000000` | 25 | |
| `0xfc000000`, `0xfff00000` | 18, 16 | |

## The firmware is QPSOS, a QPixel in-house RTOS

Module banner strings name the whole subsystem set:

```
QPSOS main            QPSOS Memory Manager     QPSOS OS Manager
QPSOS Main thread     QPSOS Settings Manager   QPSOS OS Manager: messaging
QPSOS Time Manager    QPSOS Platform Manager   QPSOS OS Manager: semaphores
QPSOS shell           QPSOS Test code          QPSOS OS Manager: event groups
QPSOS I2C
```

Platform/SoC codenames in the image: `Artesa`, `Artesa-B0`, `Artesa FPGA-V4/V5`,
`Stonegate FPGA-V4/V5`, `Beringer`, plus `use 201 emulation` — which ties to the
INF's `HKLM\SOFTWARE\qpixel\ql201`. Library banners: `FC3 Lib`, `USB Lib`,
`FAT Lib`, `SDIO Lib`, `Codec Lib`, `Codec Drv`, `ARC audio`.

## There is a full interactive debug shell

This is the most valuable thing in the image. The firmware carries a command
shell with help text for every command:

| command | purpose |
|---|---|
| `help [command]` | Display commands usage |
| `sysinfo` | Display system information |
| `versions` | Version info for all available modules |
| `clocks` | **Shows the value of various clock rates** |
| `platform` | Display current platform settings |
| `threads` | List all threads in the system |
| `messages [reset]`, `semaphores`, `events`, `timers` | OS object inspection |
| `heaps [n\|c]` | Memory heap info |
| `uptime`, `time [-s mmddyyyyhhmmss]` | |
| `settings [-] tag key [[u\|s\|b] val]` | **persistent storage** read/write |
| `i2c [type] direction addr[subaddress[writedata]] [bytes]` | **I²C access** |
| `gpio bit [[s i\|o]\|[w 0\|1]\|r\|u\|e\|d]` | GPIO direction/value/interrupts |
| `f memory` | Write to memory |
| `cache [dc]` | Data-cache clean |
| `dbgopts [-m 0\|1\|2\|3]` | 0 none, 1 local, 2 local/deferred, **3 external** |
| `test [all \| ([tim] [mem] [msg] [cpp] [m2m])]` | Self-tests |
| `kill taskID`, `crash 0\|1\|2` | |
| `switch` | **Switch to the other firmware** |
| `reboot`, `exit` | |
| `usb dump [cil\|dev\|host\|sys reg n_count] \| tell [status\|mode] \| prof \| test wq \| gpio` | USB introspection |

The `i2c` command's own help documents the `i2cType` the driver's opcodes carry:

```
i2c [type] direction addr[subaddress[writedata]] [bytes]
   type: s (SW), h (HW) (leave blank for default)
   direction: r (read), w (write)
   addr: device address in Hex (1 byte only)
   subaddress: Hex string with subaddress
   writedata: Hex string with data to write to device (write only)
   bytes (read only): number of bytes to read
```

That maps one-to-one onto the USB opcode pairs in `RE.md`: `0x05`/`0x08` are the
**HW** I²C master and `0x0B`/`0x0C` are the **SW** (GPIO bit-banged) one.

### Why our pre-firmware SW-I²C probe fails

The I²C module carries these error strings:

```
i2c.c:QPSOSI2CTransfer: Invalid i2cType: %d
i2c.c:QPSOSI2CTransfer: Invalid I2C master type: %d
i2c.c:QPSOSI2CTransfer: Software (GPIO) I2C not supported
i2c.c:QPSOSI2CTransfer: Could not lock the I2C mutex (%X)
i2c.c:QPSOSI2CTransfer: QPPFMGetAttr(I2C) returned invalid state: %d
```

So SW-I²C transfers are serviced **by the ARM firmware**, not by the USB
front-end ROM — and one build path outright rejects them. That is consistent with
two independent observations:

1. Our probe on macOS gets a well-formed 2-byte reply with status `0x06` (not the
   documented `0x08`) for *every* sub-address, i.e. the transport works but the
   transfer never happens. No firmware is loaded, so nothing services it.
2. In the Windows log the NUC100 SW-I²C traffic starts at t=9.576 s, *after* the
   firmware comes up at 9.349 s — never before.

Corollary: **HDMI status (sync, resolution, HDCP) is not reachable until the ARM
firmware is running.** It is not a missing GPIO write; see the triangulation below.

### Reaching the shell

Unknown, and worth finding out: `dbgopts -m 3` ("external") suggests the shell and
its output can be routed off-chip. Candidate transports are a board UART or a
diagnostic path over USB (`usb dump ... reg n_count` implies register dumping over
some channel). A reachable shell would let us run `clocks`, `platform`, `sysinfo`
and `i2c` interactively — effectively a supported introspection API for a device
with no documentation. It requires the firmware to be running first, so it is
gated behind DDR bring-up.

## Speculative but notable: a camera gadget

The USB subsystem contains a gadget framework with named endpoints
(`ep1in`…`ep15out`), transfer-type names (`isoc`, `bulk`, `control`), and these
gadget identities:

```
Q.cam gadget(FX2)      Gadget Zero      USB Loader      FX2 clone
```

`isoc` endpoint support plus a gadget called **Q.cam** is interesting, because an
isochronous camera gadget is the shape of a UVC device. Combined with the shell's
`switch` ("Switch to the other firmware") and the driver's
`QPCODEC_DIAG_FIRMWARE_SWITCH_MODE`, there may be an alternate personality in
which the hardware presents differently on the bus.

This is **a hypothesis from strings only.** Nothing here shows a UVC descriptor
set, and the C835's single configuration has no isochronous endpoints. Do not
plan around it — but it is cheap to check once the firmware runs, and it would be
a far better outcome than decoding the mailbox, so it is worth one look.

## Triangulation: three sources agree on the control page

The strongest validation so far. The register block at `0x600` was arrived at
independently three ways:

| source | evidence |
|---|---|
| `AVer330USB.sys` disassembly | `CQLCodec_SetGPIODefaults` writes regs `0x610` (dir) and `0x614` (val) |
| **live hardware** (macOS, libusb) | the only non-zero regs in the `0x600` page are `0x600, 0x610, 0x614, 0x618, 0x61c, 0x630, 0x634, 0x6ac, 0x6c8, 0x6cc, 0x6f8, 0x6fc` |
| `qpvidfwusb.bin` literal pools | references `0x61c` (12×), `0x614` (5×), `0x618` (2×), `0x634` (2×) |

A static guess, a live read, and an independent firmware image converging on the
same offsets means the register model is right.

## Next on the firmware

- Resolve the reset path at `0x5fbdc` and find the C entry / `QPSOS main`.
- Locate the shell's command table (the help strings are `(name, help)` pairs, so
  the table is a struct array near `0x643f0`–`0x646e0`) and recover the dispatch
  function — that names every shell handler.
- Find the mailbox handler: the firmware must poll whatever the host writes at
  `0x6cc`/`0x6fc`, so cross-reference those offsets' users.
- Decode `QPPFMGetAttr` (`pf.c`) — it gates I²C on a platform attribute and knows
  the chip version, which would explain the `0x06` status.
