# GL310 HDMI camera for macOS

A CoreMediaIO Camera Extension that shows up in every camera app (FaceTime, Zoom,
browsers, OBS…) as **"GL310 HDMI"**.

```
card --USB--> tools/gl310live --TS--> ffmpeg (VideoToolbox) --NV12--> gl310feed
                                                                        |
                         camera apps <-- source stream <-- extension <--+ sink stream
```

The extension never touches USB. It runs sandboxed as a system user. The card is driven
by the user-space tools in `tools/`, which work today. This is the same split OBS's
virtual camera uses. With no frames arriving, the camera shows a dark placeholder.

## Build

Only the Command Line Tools are needed (no Xcode):

```
macos/build.sh
```

This produces `macos/build/GL310Camera.app` (with the extension inside) and
`macos/build/gl310feed`.

## The signing problem

A Camera Extension is a **system extension**. To activate one, the container app needs the
`com.apple.developer.system-extension.install` entitlement, and macOS only honours it
with a provisioning profile. That holds even if the extension never leaves this
machine. Free "Personal Team" Apple IDs cannot get that capability. So there are two
ways to activate it:

### A. Apple Developer Program (paid): the supported way

1. In the developer portal, register two App IDs, for example
   `com.yourname.gl310.camera` (System Extension capability, App Groups) and
   `com.yourname.gl310.camera.extension` (App Groups). Add the app group
   `TEAMID.com.yourname.gl310.camera` to both, then create a development
   provisioning profile for each.
2. Build:
   ```
   TEAM_ID=TEAMID BUNDLE_PREFIX=com.yourname \
   SIGN_ID="Apple Development: you@example.com (XXXXXXXXXX)" \
   PROFILE=app.provisionprofile EXT_PROFILE=ext.provisionprofile macos/build.sh
   ```
3. Install and activate:
   ```
   cp -R macos/build/GL310Camera.app /Applications/
   /Applications/GL310Camera.app/Contents/MacOS/GL310Camera activate
   ```
   Approve it in **System Settings → General → Login Items & Extensions → Camera
   Extensions**.

### B. Without a paid account: lower SIP (not recommended)

With SIP disabled from Recovery (`csrutil disable`) and `systemextensionsctl developer
on`, macOS stops requiring the app to be in /Applications. However, an ad-hoc signature
on a restricted entitlement is still rejected by AMFI. Getting past that means
disabling AMFI as well (a boot-arg), which removes a core macOS protection for every
process on the machine. Developer forum reports also say developer mode is unreliable
for CMIO extensions on macOS 26.x. This route has not been tested here.

## Use

```
tools/gl310cam
```

Then pick **GL310 HDMI** as the camera in any app. Ctrl-C stops everything and shuts
the card down cleanly. `tools/gl310cam --dry-run` runs the whole chain without the
camera; it was verified at 30 fps with 0 dropped frames.

## Without the extension: OBS Virtual Camera

OBS ships its own camera extension, already signed, so this works today:

```
tools/gl310live | ffmpeg -loglevel error -i - -c copy -f mpegts udp://127.0.0.1:5000
```

In OBS, add a **Media Source**, untick "Local File", and set the input to
`udp://127.0.0.1:5000`. Then press **Start Virtual Camera**.

## Uninstall

```
/Applications/GL310Camera.app/Contents/MacOS/GL310Camera deactivate
rm -rf /Applications/GL310Camera.app
```
