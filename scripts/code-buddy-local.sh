#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
HELPER_BUNDLE_ID="${CODE_BUDDY_HELPER_BUNDLE_ID:-com.malvin.codebuddy.blehelper}"
HELPER_APP="$ROOT/.build/native/CodeBuddyBLEHelper.app"
CODE_BUDDY_PYTHON="/opt/homebrew/Cellar/code-buddy/0.1.4/libexec/bin/python"

if [[ ! -x "$CODE_BUDDY_PYTHON" ]]; then
  echo "code-buddy libexec python not found: $CODE_BUDDY_PYTHON" >&2
  exit 1
fi

current_bundle_id=""
if [[ -f "$HELPER_APP/Contents/Info.plist" ]]; then
  current_bundle_id="$(/usr/libexec/PlistBuddy -c "Print :CFBundleIdentifier" "$HELPER_APP/Contents/Info.plist" 2>/dev/null || true)"
fi

if [[ "${CODE_BUDDY_REBUILD_HELPER:-0}" == "1" || ! -x "$HELPER_APP/Contents/MacOS/CodeBuddyBLEHelper" || "$current_bundle_id" != "$HELPER_BUNDLE_ID" ]]; then
  CODE_BUDDY_HELPER_BUNDLE_ID="$HELPER_BUNDLE_ID" "$ROOT/scripts/build-native-ble-helper.sh" >/dev/null
fi

export CODEX_BUDDY_BLE_BACKEND=native
export CODEX_BUDDY_BLE_HELPER_APP="$HELPER_APP"
export PYTHONPATH="$ROOT/src${PYTHONPATH:+:$PYTHONPATH}"
export PATH="$HOME/.nvm/versions/node/v22.15.0/bin:/opt/homebrew/bin:/usr/local/bin:/usr/bin:/bin:/usr/sbin:/sbin"

exec "$CODE_BUDDY_PYTHON" -m codex_buddy "$@"
