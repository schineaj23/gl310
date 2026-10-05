# Capture session notes, 2026-10-05 (lynxbook, Windows 11, docked)

## Bottom line so far
- **USBPcap can't see this device's traffic.** It's 0 for 3 so far: take 1 (hot-plug), take 2
  (`pnputil /restart-device` during the capture) and a 16 s idle test, while the driver was
  polling it every 125 ms. Each time, the only GL310 packets were the descriptors Wireshark
  injects when a capture starts. Other devices on the same hub (mouse, Logitech receiver)
  were captured normally, with "Capture from all devices connected" on.
  Working theory: AVer330USB.sys sends its URBs in a way that skips USBPcap's filter
  (for example, straight to the PDO). Not proven.
- **The checked driver's DebugView log makes up for much of it.** It logs every DMA
  (ARM address, length, swap, sync), every bulk completion with its pipe and byte count,
  and every ARM mailbox message with its parameters. See take 2 below.

## Take 2: driver restart while capturing (gl310-bringup-debugview.log is now this take)
The pcap again has no GL310 traffic. The DebugView log is clean (after a reboot, so no
stale instance):

```
4.611  dispatchPnpStop / dispatchRemove      (pnputil restart)
4.886  DriverEntry -> dispatchPnpStart -> LoadFile both images
4.934  CADI7441_InitDevice (HDMI rx)  ... 5.937 SelectVideoSource(20)
5.942  CADI9889_InitDevice (HDMI tx passthrough), SetVideoStandard 720p
5.981  CQLCodec_FWDownloadAll() checkState(0) verify(1)
5.981  CUsbCntl_ResetArm() run(0)                         <- hold ARM in reset
5.995  QPHCI_ReInit() mode(1) regBase(0x100000) memBase(0x0) page(0x100000)
5.997  CQLCodec_InitializeMemory() type(1) size(512Mb)
6.050  FWDownload start(0x100000) size(363832)   = qpaudfwusb.bin (DSP)
6.110  FWDownload start(0x0)      size(454064)   = qpvidfwusb.bin (ARM), "QPSOS2"
```
DMA model, read off `CQLCodec_StartDMAWrite/Read` + `BULK Pipe(n) xfered(..)`:
- **Writes go out on pipe 0 (EP 0x02) and reads come in on pipe 2 (EP 0x81)**, in chunks
  of at most 32768 bytes during the download.
- `Arm(..)` addresses are **32-bit word addresses**: they step by 0x2000 per 32 KiB chunk.
  The audio image at byte 0x100000 is written to Arm(0x40000); the video image at 0 to Arm(0x0).
- `verify(1)`: every chunk written is immediately read back (`StartDMARead`, same Arm
  address, pipe 2) for comparison.
- Before the audio image, 96,492 bytes (32768+32768+30956) go to Arm(0x5634e..). That's
  some other blob or init region; identity unknown.
- Firmware chunks use swap(0) sync(1); stream reads use swap(1) sync(0).

Streaming model (~1,000 frames in the log):
```
ARM -> host  CEncoderTask_ProcessArmMessage() cmd(0x40) channel(0) p1(0x83) p2(armaddr) p3(0) p4(nwords) p5(..)
             p4 is the frame length in 32-bit words: 0x9cf9 * 4 = 160740 bytes
host         StartDMARead Arm(p2) len(131072) + len(remainder)   swap(1)   on pipe 2 (EP 0x81)
host -> ARM  QPFWAPI_SendMessageToARM() REG_TO_ARM_MESSAGE_STATUS(0x1) REG_TO_ARM_MESSAGE(0x30)  (ack)
```
Typical frame sizes are about 160 KB and 80-98 KB. Pipe 2 carried 1,569 bulk reads,
mostly 131072 + ~29.7 KB pairs.

**Update:** the command framing has now been recovered from the .sys; see `../RE.md`.
The original open question was the **USB framing of the commands** on pipe 1 (EP 0x04):
the 8-byte register read, the 9-byte software-I2C write-then-read, the register write,
and whatever StartDMAWrite/Read sends before the bulk data. Those formats sit in a few
small `CUsbCntl_*` functions in AVer330USB.sys, and the debug strings point straight at them.

## Take 1: gl310-bringup.pcapng + gl310-bringup-debugview.log

**The pcap is unusable: the GL310 is not in it.** The capture covered USBPcap1 and USBPcap2
(196,568 packets, 1,582 dropped), but every packet belongs to the dock's devices. 582 MB of
it is the dock's Realtek NIC (0bda:8153, bus 2 addr 6). The card enumerated during the
capture (DebugView shows `dispatchPnpStart` 8.15 s after the clear), but USBPcap recorded
none of its traffic, not even its enumeration. Afterwards Wireshark's device tree shows the
card on USBPcap1 at address 20, while the capture saw no bus-1 address above 16.
Conclusion: on this machine, "Capture from newly connected devices" didn't pick up the
card. Retake by restarting the device driver *while* capturing, instead of hot-plugging.

**The DebugView log is good.** It covers a complete cold bring-up through ~15 s of OBS
streaming. Useful findings:

### Pipe roles (from `CUsbCntl_Constructor`)
```
cmd_wr(1) cmd_rd(3) dma_wr(0) dma_rd(2)    accessMode(1)
```
Pipe index i is the i-th endpoint in the interface descriptor, and the log confirms
`pipe(1) ep(0x4)`. That gives:

| pipe | EP   | role    |
|------|------|---------|
| 0    | 0x02 | dma_wr: bulk OUT, firmware/DMA payloads (expected) |
| 1    | 0x04 | cmd_wr: command OUT |
| 2    | 0x81 | dma_rd: bulk IN, bitstream (expected) |
| 3    | 0x83 | cmd_rd: command replies |

### Command sizes seen on cmd_wr (EP 0x04)
- `CUsbCntl_RegisterRead` (QPHCI register read, used by GetGPIOBitValue): **8 bytes**
- `CUsbCntl_SWI2CWriteThenRead` (software I2C): **9 bytes**; slave 0x2a, sub 0x1b / 0x25
  (HDMI sync/HDCP status). Slave 0x2B is a Nuvoton NUC100 MCU (`InterfaceNUC100`), which
  the driver reads and writes as "Position(0xNN)" registers (0x1b sync, 0x1c, 0x25 HDCP,
  0x2c, 0x2e audio src, 0x3a, 0x40-0x44 brightness/contrast/volume).

### Bring-up order (t = seconds since log clear)
```
8.151  dispatchPnpStart; LoadFile qpvidfwusb.bin (454064) + qpaudfwusb.bin (363832)
8.184  device + config descriptors; select config; reset pipes 1,3,0,2
8.193  CQLCodec_InitDevice: "load external Video FW", "load external Audio FW"
9.240  CQLCodec_FWDownloadAll() checkState(0) verify(1)      <- firmware push + verify
9.349  CQLCodec_FWDownload() QPSOS2
9.574  CQLCodecLib_InitDevice() QPSOS2, m_USBDefaultMode(0)   (~330 ms for both images)
9.575  ProjectFactory: S-PPROM project name 0xc835 -> ProjectC875
9.576  NUC100 MCU init over I2C (slave 0x2B); volume/brightness/contrast writes
9.633  monitor thread started; "Check HDMI sync"
9.736  "Skip initialize signal info since no sync detected"
```
Then streaming: `CTask_SendIoBlock`, `CTask_CompleteArm() toHostCmd(0x40...)`,
`CEncoderTask_ProcessArmMessage`, `CChannel_TimeStamp() PTS`, MPEG out pin, H.264
(`enableMJPEG: Set back to the H264 encoding`).

### Signal
Driver reports `SyncDetected(1) resolution (1920 x 540) audio (48) hdcp (0)`, and later
`W x H = 1920 x 540, FrameRate = 60, ScanMode = Interlace`, so the camera outputs **1080i60**
with 48 kHz audio and no HDCP. (Early on it briefly reported 1280x720@60p before lock.)

### Things that pollute the captures
- **Stale driver instance.** After the card was unplugged, a second instance kept polling
  every ~750 ms and failing with `0xc000000e` (STATUS_NO_SUCH_DEVICE). It survives until
  reboot. Its lines are easy to filter, but a reboot gives a clean log.
- **Teams and WebView2 open the device by themselves.** `ms-teams.exe` and
  `msedgewebview2` created capture filters at plug-in, and WebView2 even started the
  encoder at 10.36 s, before OBS. Quit Teams (and ideally anything WebView2-based)
  before Capture B, or its start/stop messages get mixed in with OBS's.
- The dock NIC generates hundreds of MB on bus 2. Capture **USBPcap1 only**.
