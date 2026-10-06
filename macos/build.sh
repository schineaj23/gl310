#!/bin/sh
# Build GL310Camera.app (with the camera extension inside) and gl310feed.
#
#   macos/build.sh                                  # ad-hoc signed
#   TEAM_ID=ABCDE12345 SIGN_ID="Apple Development: you@example.com" macos/build.sh
#
# Needs only the Command Line Tools (swiftc + the macOS SDK), not Xcode.
#
# Activating a system extension needs the com.apple.developer.system-extension.install
# entitlement, which macOS only honours with a provisioning profile from a paid
# Apple Developer Program team. Pass PROFILE=path/to/app.provisionprofile (and
# EXT_PROFILE=... for the extension) to embed them. An ad-hoc build compiles and
# assembles correctly but will only load with SIP / AMFI relaxed - see macos/README.md.
set -e
cd "$(dirname "$0")"
TEAM_ID=${TEAM_ID:-XXXXXXXXXX}
# A paid team must register its own, unique bundle IDs: BUNDLE_PREFIX=com.yourname
BUNDLE_PREFIX=${BUNDLE_PREFIX:-local}
SIGN_ID=${SIGN_ID:--}
OUT=build
APP=$OUT/GL310Camera.app
APP_ID=$BUNDLE_PREFIX.gl310.camera
EXT_ID=$APP_ID.extension
EXT=$APP/Contents/Library/SystemExtensions/$EXT_ID.systemextension
GROUP=$TEAM_ID.$APP_ID
SWIFTC="xcrun swiftc -swift-version 5 -O -target arm64-apple-macos13.0"

rm -rf "$OUT"
mkdir -p "$APP/Contents/MacOS" "$EXT/Contents/MacOS"

echo "== compiling ($APP_ID)"
cat > "$OUT/BuildIDs.swift" <<EOF
extension GL310 { static let extensionID = "$EXT_ID" }
EOF
$SWIFTC -o "$EXT/Contents/MacOS/$EXT_ID" Shared/IDs.swift "$OUT/BuildIDs.swift" Extension/*.swift
$SWIFTC -o "$APP/Contents/MacOS/GL310Camera" Shared/IDs.swift "$OUT/BuildIDs.swift" App/main.swift
$SWIFTC -o "$OUT/gl310feed" Shared/IDs.swift "$OUT/BuildIDs.swift" Feeder/main.swift

echo "== bundles"
cat > "$APP/Contents/Info.plist" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
  <key>CFBundleIdentifier</key><string>$APP_ID</string>
  <key>CFBundleName</key><string>GL310Camera</string>
  <key>CFBundleExecutable</key><string>GL310Camera</string>
  <key>CFBundlePackageType</key><string>APPL</string>
  <key>CFBundleShortVersionString</key><string>0.1</string>
  <key>CFBundleVersion</key><string>1</string>
  <key>LSMinimumSystemVersion</key><string>13.0</string>
  <key>LSUIElement</key><true/>
</dict></plist>
EOF
cat > "$EXT/Contents/Info.plist" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
  <key>CFBundleIdentifier</key><string>$EXT_ID</string>
  <key>CFBundleName</key><string>GL310 HDMI Camera</string>
  <key>CFBundleExecutable</key><string>$EXT_ID</string>
  <key>CFBundlePackageType</key><string>SYSX</string>
  <key>CFBundleShortVersionString</key><string>0.1</string>
  <key>CFBundleVersion</key><string>1</string>
  <key>LSMinimumSystemVersion</key><string>13.0</string>
  <key>NSSystemExtensionUsageDescription</key>
  <string>Makes the GL310 / C835 HDMI capture card available as a camera.</string>
  <key>CMIOExtension</key><dict>
    <key>CMIOExtensionMachServiceName</key><string>$GROUP.service</string>
  </dict>
</dict></plist>
EOF

cat > "$OUT/app.entitlements" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
  <key>com.apple.developer.system-extension.install</key><true/>
  <key>com.apple.security.application-groups</key><array><string>$GROUP</string></array>
</dict></plist>
EOF
cat > "$OUT/ext.entitlements" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
  <key>com.apple.security.app-sandbox</key><true/>
  <key>com.apple.security.application-groups</key><array><string>$GROUP</string></array>
</dict></plist>
EOF

[ -n "$EXT_PROFILE" ] && cp "$EXT_PROFILE" "$EXT/Contents/embedded.provisionprofile"
[ -n "$PROFILE" ] && cp "$PROFILE" "$APP/Contents/embedded.provisionprofile"

echo "== signing with identity '$SIGN_ID', team '$TEAM_ID'"
codesign --force --options runtime --timestamp=none --entitlements "$OUT/ext.entitlements" \
         -s "$SIGN_ID" "$EXT"
codesign --force --options runtime --timestamp=none --entitlements "$OUT/app.entitlements" \
         -s "$SIGN_ID" "$APP"
codesign --force -s "$SIGN_ID" "$OUT/gl310feed"
codesign --verify --strict --deep "$APP" && echo "signature structure OK"

echo
echo "built: $APP"
echo "       $OUT/gl310feed"
