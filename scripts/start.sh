#!/usr/bin/env bash
set -euo pipefail

ROOT="/Users/malvin/Coding/opensource/CodeBuddy"

export CODEX_BUDDY_BLE_HELPER_APP="$ROOT/.build/native/CodeBuddyBLEHelper.app"
export CODEX_BUDDY_BLE_BACKEND=native
export PYTHONPATH="$ROOT/src${PYTHONPATH:+:$PYTHONPATH}"
export PATH="$HOME/.nvm/versions/node/v22.15.0/bin:/opt/homebrew/bin:/usr/local/bin:/usr/bin:/bin:/usr/sbin:/sbin"

exec /opt/homebrew/Cellar/code-buddy/0.1.4/libexec/bin/python -m codex_buddy "$@"
