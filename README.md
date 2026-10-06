# gl310

An open driver for the **AVerMedia Live Gamer Portable Lite** (GL310, USB `07ca:c835`), a
USB 2.0 HDMI capture card with a hardware H.264 encoder. AVerMedia's own macOS support
ended with macOS 10.13. This project drives the card from user space with libusb and turns
it into a webcam on modern macOS.

| | status |
|---|---|
| Live H.264 / MPEG-TS stream (1080p30) | works on macOS; the libusb tools should port to Linux but are untested there |
| macOS webcam ("GL310 HDMI" in any app) + menu-bar app | works; see [docs/macos.md](docs/macos.md) |
| Linux webcam (v4l2loopback) | not done yet |
| Audio | not implemented (the stream declares an AAC track but it carries no data) |

## Firmware

The card runs firmware uploaded by the host at every session. The two images come from
AVerMedia's Windows driver package and are expected in `vendor/` (`qpvidfwusb.bin`,
`qpaudfwusb.bin`). Set `GL310_FIRMWARE=/some/dir` to load them from somewhere else.

## Build (macOS)

Requirements: Apple Silicon Mac, macOS 13 or later, the Command Line Tools
(`xcode-select --install`) and Homebrew packages:

```
brew install libusb pkg-config ffmpeg
```

Build the command-line tools, then the camera extension and the menu-bar app:

```
make            # tools/: gl310i2c, gl310init, gl310start, gl310log, gl310recover
make macos      # macos/build/: GL310Camera.app (contains the camera extension),
                # GL310Menu.app, gl310feed, gl310probe-cam
```

Install the apps and activate the camera extension:

```
cp -R macos/build/GL310Camera.app macos/build/GL310Menu.app /Applications/
/Applications/GL310Camera.app/Contents/MacOS/GL310Camera activate
```

Activation needs either an Apple Developer Program signing identity or SIP disabled.
macOS only loads system extensions with a provisioning-profile entitlement, even ones
built for your own machine. Both routes are described in
[docs/macos.md](docs/macos.md#install).

## Use

- **Webcam:** open **GL310Menu**, press **Start Camera**, and pick "GL310 HDMI" in any
  app. The menu also sets aspect (16:9 or 4:3 crop), 1080p/720p and bitrate.
- **From a terminal:**
  - `tools/gl310cam [--res 1080|720] [--aspect 16:9|4:3] [--rate KBPS]` runs the webcam
  - `tools/gl310live | ffplay -fflags nobuffer -` shows the raw stream (MPEG-TS on
    stdout; works without the extension)

Ctrl-C stops either one and shuts the card down cleanly.

## Documentation

- [docs/macos.md](docs/macos.md): installing, signing, settings, troubleshooting, the OBS
  alternative
- [docs/protocol.md](docs/protocol.md): how the card is driven (USB commands, firmware
  boot, mailbox, encoder configuration, stream format)
- [research/](research/README.md): the reverse-engineering notes, captures and probe
  tools
