# GL310 / C835 macOS driver — recon findings

Goal: use an AVerMedia GL310 (C835) HDMI capture card as a webcam on macOS 26.x
(Apple Silicon), while docked. No vendor macOS driver exists past macOS 10.13.

Recon date: 2026-10-05. Verdict: **tractable**, gated on one USB bus capture.

## 1. The device as macOS sees it

Enumerated and healthy on the dock's USB 2.0 hub branch, high speed (480 Mbps):

```
DEVICE 07ca:c835  bcdUSB=0200  class=0/0/0  numCfg=1
  CONFIG 1: numIfaces=1  maxPower=500mA
   INTERFACE 0: alt=0  class=255  sub=0  proto=0  numEP=4
      EP 0x02 OUT  BULK   wMaxPacket=512  bInterval=3
      EP 0x04 OUT  BULK   wMaxPacket=512  bInterval=3
      EP 0x81 IN   BULK   wMaxPacket=512  bInterval=3
      EP 0x83 IN   BULK   wMaxPacket=512  bInterval=3
```

- Serial `201360500216`, IOKit path `Aver_C835_USB@00111000`, locationID `0x111000`.
- `bInterfaceClass = 255` → **vendor-specific**. A UVC camera is class 14 with
  isochronous endpoints. There are **zero** isochronous endpoints here.
- Four bulk pipes + no isoc = onboard **hardware H.264 encoder** emitting a
  compressed bitstream, driven by a proprietary command protocol.
- Consequence: macOS has nothing to bind to. `AppleUSBVideoSupport` correctly
  ignores it. This genuinely requires implementing the vendor protocol — there is
  no quirk to patch and no UVC mode to unlock.
- Only two `AppleUSBHostDeviceUserClient`s are attached (Google Chrome's WebUSB
  enumerator and Homebrew `display_switch`). Neither claims the interface, so
  libusb can claim interface 0 without a fight.

Reproduce with `tools/desc.c`:
`clang -o desc tools/desc.c -I/opt/homebrew/include -L/opt/homebrew/lib -lusb-1.0`

## 2. Prior art: none usable

- AVerMedia's macOS support ends at 10.13. Nothing to port.
- No Linux/V4L2 driver for `07ca:c835`.
- `github.com/Trouffman/octv_gears_lgp` (for the *related* C875) was cancelled
  before a working solution. Its README recommends buying a UVC dongle instead.

So the protocol has to come from the Windows driver.

## 3. The Windows driver package — this is what makes it feasible

Archived at `archive.org/details/aver-media-gl-310-usb-hd-capture-device-v-3.2802.64.40`
(1.9 MB). Copies of the important files are in `vendor/`.

| File | What it is |
|---|---|
| `AVer330USB.sys` | 1.5 MB PE32+ x86-64 kernel driver — **checked/debug build** |
| `qpvidfwusb.bin` | 454 KB **raw ARM32 firmware**, load base `0x0` |
| `qpaudfwusb.bin` | 364 KB **DSP firmware** (audio encoder) |
| `AVer835_x64.inf` | Install rules, registry layout, firmware registration |
| `PropPage.ax` | DirectShow property page (PE32 x86 DLL) |

### 3a. Firmware ships unencrypted — the usual project-killer is absent

`qpvidfwusb.bin` opens with `18f0 9fe5` repeated eight times = `LDR PC,[PC,#0x18]`,
a textbook **ARM exception vector table** (reset / undef / SWI / pabort / dabort /
reserved / IRQ / FIQ). Slot 5 is `feffffea` = `B .` (branch-to-self), exactly as
expected for the reserved vector. A literal pool at `0x20` holds `0x0005fbdc`,
`0x0005fb90`, … — consistent with a flat image of 454064 bytes (`0x6EDB0`) loaded
at base `0x00000000`.

`qpaudfwusb.bin` is fixed 8-byte words (`2020800f1000 XXXX` with varying tail) —
a DSP vector table, 64-bit instruction words.

Both are plain blobs we can upload ourselves. **The device needs a firmware push
before it will do anything**, which is why it sits inert on macOS today.

### 3b. The driver is a *checked build* — full debug strings

PDB path embedded in the binary:

```
c:\users\a003889.avermedia\driver\c875\new\driver\avermedia\
  bda_v3.03.0.1\api\qpavstrm\obj\chk_win7_amd64\amd64\AVer330USB.pdb
```

`chk_win7_amd64` = checked build, so every internal function name and its
parameter list survives as a `DbgPrint` format string. 22k strings are dumped to
`vendor/AVer330USB.strings.txt`. This is effectively a map of the driver's
internals, free of charge. Examples:

```
CQLCodecLib_Set() QPCODEC_DIAG_FIRMWARE_DOWNLOAD size(%d) addr(0x%x) reset(%d)
HDMI information : status (%d) resolution (%d x %d) audio (%d) hdcp (%d)
BULK Pipe(%d) %8d xfered(%8d) Status(0x%x) USB status(0x%x)
CEncoderTask_ProcessArmMessage() cmd(0x%x) channel(%d) p1(0x%x) ... p5(0x%x)
```

Note the firmware download signature: **(buffer, size, load address, reset flag)** —
matching the ARM-image-at-base-0 reading above, with an explicit reset release.

### 3c. The silicon is identified

`AVer835_x64.inf` registers settings under `HKLM\SOFTWARE\qpixel\ql201`.
The encoder SoC is a **QPixel QL201** — hence the `QP*` / `QL*` prefixes
throughout the driver. Firmware is registered as:

```
HKR,Platform,AudioFirmware,,\SystemRoot\system32\drivers\qpaudfwusb.bin
HKR,Platform,VideoFirmware,,\SystemRoot\system32\drivers\qpvidfwusb.bin
HKLM,SOFTWARE\qpixel\ql201, StartInitArmLoop,0x00010001,0
```

Two PIDs share one install section: `c835` (ours) and `d835`. The INF also
carries a `%AVer.Decoder%` / `USBDecFriendlyName`, so `d835` is likely the
encoder+decoder variant.

### 3d. The analog/HDMI front end is all public Analog Devices parts

Driver class names reveal the I2C-attached front end:

| Part | Role | Open docs |
|---|---|---|
| **ADV7614** | HDMI **receiver** (the input path) | datasheet + Linux `adv7604.c` covers this family |
| ADV7401 | multiformat video decoder (analog/component in) | public datasheet |
| ADV7441 | video + HDMI decoder | public datasheet |
| ADV7393 | video encoder / DAC (analog out) | public datasheet |
| AD9889 | HDMI transmitter (passthrough out) | public datasheet |

This is a large de-risking: "which registers do I poke to get HDMI sync, read the
input resolution, and check HDCP" is answered by public datasheets and existing
open-source drivers, **not** by reverse engineering.

### 3e. The protocol is a small orthogonal primitive set

Rather than a sprawling opaque command set, the driver talks to the device through
a handful of `QPCODEC_DIAG_*` primitives tunnelled over the bulk pipes:

```
FIRMWARE_DOWNLOAD / _PTR    push a blob to an address
FIRMWARE_RELOAD             re-push
FIRMWARE_SWITCH_MODE        mode change
FIRMWARE_COMMAND            mailbox to the ARM firmware  <-- encoder control
RESET_ARM / HW_RESET        reset control
MEM_READ / MEM_WRITE        arbitrary ARM memory access
DMA_READ / DMA_WRITE (+_PTR, _PTR_EX)   bulk DMA
I2C_READ / I2C_WRITE_THEN_READ / HCI_REG_READ   I2C bridge to the ADI parts
EMULATION_ON / EMULATION_OFF
```

Plus a thin USB layer: `QLUSBFW_I2C_READ`, `QLUSBFW_SW_I2C_READ`.

Bring-up therefore looks like: reset ARM → download video FW to its load address →
download audio FW → release reset → bring up ADV7614 over the I2C bridge → poll
HDMI status for resolution/HDCP → issue encoder-start via `FIRMWARE_COMMAND` →
drain H.264 from a bulk IN pipe.

## 4. Remaining unknowns (the actual risk)

1. **The bulk wire format** — the exact packet header wrapping those primitives on
   EP `0x02`/`0x81`, and which of the two IN pipes carries the bitstream vs.
   status/mailbox replies.
2. **The ARM mailbox message format** for encoder control (start/stop, bitrate,
   resolution), i.e. the `cmd/channel/p1..p5` struct in `ProcessArmMessage`.
3. **Latency.** Unknowable until it streams. Hardware encoders in this family
   carry real pipeline delay — it is precisely why the device has HDMI
   passthrough. This is the main *product* risk: the driver can succeed
   technically and still feel bad on a video call because your face drifts
   against audio from the Mac's built-in mic.

Unknowns 1 and 2 both collapse against **one good USB bus capture** on an x86
Windows PC (available). Because the driver is a checked build, the capture can be
correlated against named functions in `DbgView` output — close to ideal RE
conditions.

## 5. Target architecture — macOS *and* Linux, no kernel code on either

Scope is both platforms. That costs almost nothing, because libusb is portable and
the protocol is the only hard part: one protocol core, two thin output sinks.

```
                 ┌───────────────────────────────────────┐
                 │  libgl310  (portable C, libusb-1.0)   │
                 │  · claim iface 0, bulk 0x02/0x81      │
                 │  · firmware upload (ARM + DSP)        │
                 │  · QPCODEC_DIAG_* primitives          │
                 │  · ADV7614 bring-up via I2C bridge    │
                 │  · HDMI status poll (res / HDCP)      │
                 │  · drain H.264 Annex-B                │
                 └───────────────┬───────────────────────┘
                                 │  H.264 ES + format events
                 ┌───────────────┴───────────────┐
                 ▼                               ▼
   ┌───────────────────────────┐   ┌───────────────────────────┐
   │ macOS                     │   │ Linux                     │
   │ decode: VideoToolbox      │   │ decode: libavcodec/VAAPI  │
   │ sink:   CMIOExtension     │   │ sink:   v4l2loopback      │
   │  → Zoom/Meet/FaceTime     │   │  → /dev/videoN, any app   │
   └───────────────────────────┘   └───────────────────────────┘
```

Everything is user space on both platforms. **SIP stays enabled; no kext, no
out-of-tree kernel module.**

Deliberate choices:

- **One decoder first, accelerated decoders later.** Start with `libavcodec` on
  both platforms so there is a single code path to debug, then swap in
  VideoToolbox / VAAPI only if latency measurements demand it. Getting a correct
  frame out matters more than getting a fast one.
- **Linux sink is `v4l2loopback`, not a kernel driver.** A user-space daemon
  writing into a loopback node gets us a working `/dev/videoN` for every app with
  zero kernel code. A real in-kernel V4L2 driver is the "do it properly and
  upstream it" option and can come later, reusing the same protocol knowledge.
- **Protocol core stays dependency-light C** so it can later be lifted into a
  kernel driver or bound from other languages without a rewrite.

macOS packaging note: the host currently has only Command Line Tools. A
`.systemextension` must be signed, so shipping the Camera Extension wants full
Xcode and a Developer ID; local development can run with
`systemextensionsctl developer on`.

Linux note: no udev/permission drama expected — a udev rule granting the user
access to `07ca:c835` is enough, since nothing else claims the vendor-specific
interface.

## 6. Next step

**Runbook page (open this on the Windows machine):**
<https://claude.ai/artifact/AvHokFVqKLNfRZcZzJqBJx>

It has the full procedure with checkboxes, the exact registry command, the
Wireshark filters, and a troubleshooting section. Summary of it:

1. Install the GL310 driver from `vendor/` (or the archived installer).
2. Install **USBPcap** + Wireshark. Start capture on the GL310's root hub
   *before* plugging the device in, so enumeration and firmware upload are caught.
3. Plug in the GL310 with the camera attached and a live HDMI signal.
4. Open AVerMedia's capture app (or any DirectShow app), start a preview, let it
   run ~10 s, stop, close.
5. Also run **DebugView** (Sysinternals) with kernel capture enabled — the checked
   driver prints its own narration, which annotates the bus trace.
6. Save the `.pcapng` + DebugView log.

That single capture is the highest-leverage artifact in the whole project: it
yields the wire format, the firmware-download framing, the I2C bring-up sequence,
and the encoder-start mailbox messages in one shot.

## Pragmatic alternative

A ~$20 UVC-class HDMI capture dongle satisfies the stated goal — camera as webcam
while docked — in five minutes, with zero code and lower latency. The GL310 driver
is worth building if the project itself is the point, not because it is the
cheapest route to a working webcam.
