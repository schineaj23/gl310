# Research

How the driver was reverse-engineered. None of this is needed to *use* the card; for
that, see the main [README](../README.md) and [docs/protocol.md](../docs/protocol.md).

## Notes

| file | |
|---|---|
| [notes/RE.md](notes/RE.md) | The main journal, in chronological order: USB protocol, firmware boot, mailbox, encoder, the H.264 investigation, buffers, latency and configurations. Wrong turns are kept and corrected in place, so read it with that in mind. |
| [notes/RECON.md](notes/RECON.md) | Initial recon: USB descriptors and the Windows driver's architecture |
| [notes/FIRMWARE.md](notes/FIRMWARE.md) | First-pass analysis of the ARM firmware `qpvidfwusb.bin` |
| [notes/capture-runbook.html](notes/capture-runbook.html) | Procedure for capturing the Windows driver's traffic |

## Captures

[captures/](captures/) holds data from the working Windows setup:
- the driver's DebugView log (`gl310-bringup-debugview.log`, the source of the encoder
  configuration)
- a USBPcap trace
- a sample TS
- session notes

## Tools

The exploratory programs. Build the C ones with `make research` from the repo root.

| tool | what it does |
|---|---|
| `gl310probe.c` | checks the recovered command protocol on real hardware |
| `gl310mbox.c` | exercises the host↔ARM mailbox |
| `gl310ddr.c` | DDR init and a memory test (training turned out not to be needed) |
| `gl310life.c` | does the ARM change DRAM when it's released from reset? |
| `gl310armtest.c` | does the ARM execute code we place in DRAM? |
| `gl310aperture.c` | measures the address translation of the host interface's paging window |
| `desc.c` | dumps the USB descriptors |
| `fwarm.py` | firmware analysis (capstone) |
| `sysmap.py` | navigates `AVer330USB.sys` by its debug strings |
| `gl310cap.py` | decodes USBPcap captures of the card; [test/make_synth.py](test/make_synth.py) builds a synthetic capture to self-test it |
| `preflight.ps1` | read-only readiness check before a Windows capture |

Ghidra projects (`ghidra/`, `gl310.gpr`, `gl310.rep/`) are kept locally and are not in
git.
