# macOS: the GL310 as a webcam

A CoreMediaIO Camera Extension makes the card appear in every camera app (FaceTime,
Zoom, browsers, OBS…) as **"GL310 HDMI"**. A menu-bar app starts and stops it and
picks the settings.

```
card --USB--> tools/gl310live --TS--> ffmpeg (VideoToolbox) --NV12--> gl310feed
                                                                         |
                         camera apps <-- source stream <-- extension <---+ sink stream
```

The extension never touches USB, because it runs sandboxed as a system user. The card is
driven by the command-line tools in `tools/`, and decoded frames are pushed into the
extension. This is the same design OBS's virtual camera uses.

## Requirements

- macOS 13 or later, Apple Silicon (the build targets arm64)
- Command Line Tools (`xcode-select --install`); Xcode is not needed
- Homebrew: `brew install libusb pkg-config ffmpeg`
- the firmware in `vendor/` (see the main [README](../README.md#firmware))

## Build

```
make            # command-line tools in tools/
make macos      # macos/build/: GL310Camera.app (with the extension), GL310Menu.app,
                # gl310feed, gl310probe-cam
```

`make macos` runs `macos/build.sh`, which signs ad hoc by default. The variables it reads
for real signing are below.

## Install

A camera extension is a **system extension**. macOS only activates one when the
installing app has the `com.apple.developer.system-extension.install` entitlement, and
that requires a provisioning profile. This applies even if the extension never leaves
your machine. There are two ways to satisfy it.

### Option A: Apple Developer Program (supported, paid)

1. Register two App IDs: `com.you.gl310.camera` (System Extension and App Groups
   capabilities) and `com.you.gl310.camera.extension` (App Groups). Give both the app
   group `TEAMID.com.you.gl310.camera`, and create a development provisioning profile
   for each.
2. Build signed:
   ```
   TEAM_ID=TEAMID BUNDLE_PREFIX=com.you \
   SIGN_ID="Apple Development: you@example.com (XXXXXXXXXX)" \
   PROFILE=app.provisionprofile EXT_PROFILE=ext.provisionprofile macos/build.sh
   ```
3. Continue with **Activate** below.

### Option B: SIP disabled (no paid account)

This works with the ad-hoc build. It was tested on macOS 26.6. It lowers your Mac's
security, so turn it back on when you're done.

1. Boot into Recovery (hold the power button → Options), open Terminal, run
   `csrutil disable`, and restart.
2. `sudo systemextensionsctl developer on`

To undo it later: `sudo systemextensionsctl developer off`, then `csrutil enable` in
Recovery.

### Activate

```
cp -R macos/build/GL310Camera.app macos/build/GL310Menu.app /Applications/
/Applications/GL310Camera.app/Contents/MacOS/GL310Camera activate
```

Approve the extension if asked: **System Settings → General → Login Items & Extensions →
Camera Extensions**. After a rebuild, run `activate` again; it replaces the installed
version.

## Use

Open **GL310Menu** (the camera icon in the menu bar). From there you can start and stop
the camera and change its settings. Changing a setting while it's live restarts the
stream, which takes a few seconds. If the size changed, apps showing the camera need
to reopen it.

| setting | options | |
|---|---|---|
| Aspect | 16:9, 4:3 | 4:3 crops a 4:3 picture out of a pillarboxed 16:9 signal (240 px bars each side), giving a native 1440x1080 camera |
| Resolution | 1080p, 720p | the card always encodes 1080p; 720p is scaled on the Mac |
| Bitrate | 2, 4, 8, 12, 20, 30, 40 Mbps | every preset was verified on the card; 60 Mbps loses frames |
| Frame rate | 30 fps | fixed by the card |

The menu app runs the tools from this checkout (the path is built in), so rebuild it if
you move the repository.

From a terminal, the same thing:

```
tools/gl310cam [--res 1080|720] [--aspect 16:9|4:3] [--rate KBPS]
```

Ctrl-C stops it and shuts the card down cleanly. `--dry-run` runs the whole chain
without the camera.

## Troubleshooting

- **Log:** `~/Library/Logs/gl310cam.log` (menu → Show Log).
- **"claim failed":** something else is using the card, such as another `gl310cam`.
- **Card stops answering** (every command times out): run `tools/gl310recover`. If that
  fails, unplug and replug the card. Don't use `gl310recover --hard` on macOS; a USB reset
  can leave the card unopenable until it's replugged.
- **Delivery check:** `macos/build/gl310probe-cam 30` opens the camera the way an app
  would, and prints its format, frame rate and delivery delay.

## Without the extension: OBS Virtual Camera

OBS ships its own signed camera extension, so this needs no signing:

```
tools/gl310live | ffmpeg -loglevel error -i - -c copy -f mpegts udp://127.0.0.1:5000
```

In OBS, add a **Media Source** with "Local File" unticked and the input set to
`udp://127.0.0.1:5000`, then press **Start Virtual Camera**.

## Uninstall

```
/Applications/GL310Camera.app/Contents/MacOS/GL310Camera deactivate
rm -rf /Applications/GL310Camera.app /Applications/GL310Menu.app
```
