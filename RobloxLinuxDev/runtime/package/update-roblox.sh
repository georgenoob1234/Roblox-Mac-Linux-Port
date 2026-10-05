#!/bin/sh
# Download the current official Intel macOS client and prepare its shaders.
set -eu
umask 077
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
DATA=$HERE/DO_NOT_SHARE
unset ROBLOX_MAC_DATA ROBLOX_MAC_CONFIG ROBLOX_MAC_BROWSER_DATA
mkdir -p "$DATA"
chmod 700 "$DATA"
export HOME=$DATA/home XDG_CONFIG_HOME=$DATA/config
export XDG_DATA_HOME=$DATA/share XDG_CACHE_HOME=$DATA/cache
export TMPDIR=$DATA/tmp TMP=$DATA/tmp TEMP=$DATA/tmp
mkdir -p "$HOME" "$XDG_CONFIG_HOME" "$XDG_DATA_HOME" "$XDG_CACHE_HOME" "$TMPDIR"

progress() {
    fd=${ROBLOX_MAC_PROGRESS_FD:-}
    case "$fd" in ''|*[!0-9]*) return 0;; esac
    python3 - "$1" "$2" "$3" "${4:-0}" "${5:-0}" "${6:-true}" <<'PY' >&$fd 2>/dev/null || true
import json, sys
print(json.dumps({"phase": sys.argv[1], "percent": int(sys.argv[2]),
                  "message": sys.argv[3], "bytes_done": int(sys.argv[4]),
                  "bytes_total": int(sys.argv[5]),
                  "cancellable": sys.argv[6].lower() == "true"}, separators=(",", ":")))
PY
}
cancelled() { [ -n "${ROBLOX_MAC_CANCEL_FILE:-}" ] && [ -e "$ROBLOX_MAC_CANCEL_FILE" ]; }
if [ "${ROBLOX_MAC_LOCK_HELD:-0}" != 1 ]; then
    exec 9>"$DATA/instance.lock"
    if ! flock -n -E 73 9; then
        echo 'Close Roblox before updating.' >&2
        exit 1
    fi
fi
# Serialize updates for this release folder.
exec 8<"$HERE"
if ! flock -n -E 74 8; then
    echo 'An update is already running.' >&2
    exit 1
fi
version=$(sh "$HERE/run.sh" --client-version)
# Extract-and-run may print filesystem progress before the application's output.
version=$(printf '%s\n' "$version" | sed -n '/^version-[0-9a-f][0-9a-f]*$/p')
[ -n "$version" ] || { echo 'Could not read the current Roblox version.' >&2; exit 1; }
if [ "$version" = "$(cat "$HERE/RobloxVersion/.version" 2>/dev/null || true)" ] &&
   [ -x "$HERE/RobloxVersion/RobloxPlayer.app/Contents/MacOS/RobloxPlayer" ] &&
   [ -f "$HERE/RobloxVersion/spv-cache-v1/report.json" ]; then
    echo 'Roblox is already up to date.'
    exit 0
fi
tmp=$(mktemp -d "$DATA/.update.XXXXXX")
cleanup() {
    if [ -d "$tmp/previous" ] && [ ! -e "$HERE/RobloxVersion" ]; then
        mv "$tmp/previous" "$HERE/RobloxVersion"
    fi
    rm -rf -- "$tmp"
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM HUP
progress downloading 25 'Starting Roblox download' 0 0 true
if cancelled; then progress cancelled 0 'Update cancelled' 0 0 false; exit 125; fi
# AppImage owns the downloader and shader tools, keeping this folder small.
ROBLOX_MAC_DATA="$tmp" ROBLOX_MAC_PROGRESS_FD="${ROBLOX_MAC_PROGRESS_FD:-}" \
    ROBLOX_MAC_CANCEL_FILE="${ROBLOX_MAC_CANCEL_FILE:-}" sh "$HERE/run.sh" --download-client
if cancelled; then progress cancelled 0 'Update cancelled' 0 0 false; exit 125; fi
progress verifying 74 'Verifying the downloaded client' 0 0 true
if ! ROBLOX_MAC_DATA="$tmp/runtime" ROBLOX_MAC_APP="$tmp/RobloxPlayer.app" \
    ROBLOX_MAC_CANCEL_FILE="${ROBLOX_MAC_CANCEL_FILE:-}" sh "$HERE/run.sh" --debug --prepare-shaders ||
   [ ! -f "$tmp/spv-cache-v1/report.json" ]; then
    echo 'Shader preparation failed; the previous client has not been changed.' >&2
    exit 1
fi
if cancelled; then progress cancelled 0 'Update cancelled' 0 0 false; exit 125; fi
progress preparing 90 'Preparing shaders' 0 0 true
rm -rf -- "$tmp/runtime"
mkdir "$tmp/next"
mv "$tmp/RobloxPlayer.app" "$tmp/spv-cache-v1" "$tmp/.version" "$tmp/next/"
[ ! -f "$tmp/.display-version" ] || mv "$tmp/.display-version" "$tmp/next/"
[ ! -e "$HERE/RobloxVersion" ] || mv "$HERE/RobloxVersion" "$tmp/previous"
mv "$tmp/next" "$HERE/RobloxVersion"
progress update 100 'Update complete' 0 0 false
echo "Roblox updated: $(cat "$HERE/RobloxVersion/.version")"
