#!/bin/zsh
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SRC="$ROOT/src/codex_buddy/native_ble_helper/CodeBuddyBLEHelper.swift"
PLIST="$ROOT/src/codex_buddy/native_ble_helper/Info.plist"
APP="$ROOT/.build/native/CodeBuddyBLEHelper.app"
BIN="$APP/Contents/MacOS/CodeBuddyBLEHelper"
APP_PLIST="$APP/Contents/Info.plist"
HELPER_BUNDLE_ID="${CODE_BUDDY_HELPER_BUNDLE_ID:-com.charlex.codebuddy.blehelper}"

rm -rf "$APP"
mkdir -p "$APP/Contents/MacOS"
cp "$PLIST" "$APP_PLIST"
/usr/libexec/PlistBuddy -c "Set :CFBundleIdentifier $HELPER_BUNDLE_ID" "$APP_PLIST"
swiftc -parse-as-library -O -framework AppKit -framework CoreBluetooth \
  -Xlinker -sectcreate -Xlinker __TEXT -Xlinker __info_plist -Xlinker "$APP_PLIST" \
  "$SRC" -o "$BIN"
xattr -cr "$APP"
codesign --force --sign - "$APP"
echo "$APP"
