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
# Fixed, predictable staging beside the client so an interrupted swap can be
# found and repaired on the next launch. It must never live in DO_NOT_SHARE.
WORK=$HERE/.update-work
# A run that was interrupted leaves a durable marker in $WORK. The launcher
# repairs that on startup; refuse to build on an ambiguous state here rather
# than discard the only copy of the client. Otherwise staging is disposable.
if [ ! -e "$HERE/RobloxVersion" ] && [ -e "$WORK/TXN" ]; then
    echo 'An interrupted update is pending; run the launcher to repair it.' >&2
    exit 1
fi
rm -rf -- "$WORK"
# A release run confines ROBLOX_MAC_DATA to DO_NOT_SHARE; staging beside the
# client is accepted only as the alias's data directory. Download there, then
# adopt the fixed "next" name the swap and the launcher's recovery expect.
mkdir -p "$WORK/data"
cleanup() {
    # Never delete the backup until the new client is confirmed installed. If
    # the swap failed, restore the previous client first; keep the work dir for
    # the launcher's recovery when no valid client can be confirmed.
    if [ ! -e "$HERE/RobloxVersion" ] && [ -d "$WORK/previous" ]; then
        mv "$WORK/previous" "$HERE/RobloxVersion" 2>/dev/null || true
    fi
    if [ -x "$HERE/RobloxVersion/RobloxPlayer.app/Contents/MacOS/RobloxPlayer" ] &&
       [ -f "$HERE/RobloxVersion/.version" ] &&
       [ -f "$HERE/RobloxVersion/spv-cache-v1/report.json" ]; then
        rm -rf -- "$WORK"
    fi
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM HUP
progress downloading 25 'Starting Roblox download' 0 0 true
if cancelled; then progress cancelled 0 'Update cancelled' 0 0 false; exit 125; fi
# AppImage owns the downloader and shader tools; stage the new client directly
# beside the client so it is verified before anything is renamed.
_download_alias=$(realpath -m -- "$WORK")
ROBLOX_MAC_ALIAS_DIR="$_download_alias" ROBLOX_MAC_DATA="$_download_alias/data" \
    ROBLOX_MAC_PROGRESS_FD="${ROBLOX_MAC_PROGRESS_FD:-}" \
    ROBLOX_MAC_CANCEL_FILE="${ROBLOX_MAC_CANCEL_FILE:-}" sh "$HERE/run.sh" --download-client
mv "$WORK/data" "$WORK/next"
if cancelled; then progress cancelled 0 'Update cancelled' 0 0 false; exit 125; fi
progress verifying 74 'Verifying the downloaded client' 0 0 true
if ! ROBLOX_MAC_DATA="$WORK/next/runtime" ROBLOX_MAC_APP="$WORK/next/RobloxPlayer.app" \
    ROBLOX_MAC_CANCEL_FILE="${ROBLOX_MAC_CANCEL_FILE:-}" sh "$HERE/run.sh" --debug --prepare-shaders ||
   [ ! -f "$WORK/next/spv-cache-v1/report.json" ]; then
    echo 'Shader preparation failed; the previous client has not been changed.' >&2
    exit 1
fi
if cancelled; then progress cancelled 0 'Update cancelled' 0 0 false; exit 125; fi
progress preparing 90 'Preparing shaders' 0 0 true
rm -rf -- "$WORK/next/runtime"
# Verify the staged client in full before committing anything.
if [ ! -x "$WORK/next/RobloxPlayer.app/Contents/MacOS/RobloxPlayer" ] ||
   [ ! -f "$WORK/next/.version" ] ||
   [ ! -f "$WORK/next/spv-cache-v1/report.json" ]; then
    echo 'The downloaded client is incomplete; the previous client has not been changed.' >&2
    exit 1
fi
# The swap must be an atomic rename(2). Staging sits under $HERE on the same
# filesystem; refuse the update rather than fall back to a non-atomic copy.
if [ "$(stat -c %d -- "$WORK")" != "$(stat -c %d -- "$HERE")" ]; then
    echo 'Update staging is on a different filesystem; refusing a non-atomic swap.' >&2
    exit 1
fi
# Commit point: durably record the swap before the renames so a crash between
# them is recoverable. The marker is synced along with its directory entry.
printf 'committed %s %s\n' "$(cat "$WORK/next/.version")" "$(date +%s)" > "$WORK/TXN"
python3 - "$WORK/TXN" <<'PY'
import os, sys
fd = os.open(sys.argv[1], os.O_RDONLY)
os.fsync(fd)
os.close(fd)
d = os.open(os.path.dirname(sys.argv[1]), os.O_RDONLY)
os.fsync(d)
os.close(d)
PY
[ ! -e "$HERE/RobloxVersion" ] || mv "$HERE/RobloxVersion" "$WORK/previous"
if [ -n "${ROBLOX_MAC_TEST_CRASH_AFTER_RENAME1:-}" ]; then
    # Test hook: crash in the window between the two swap renames. SIGKILL so no
    # cleanup trap runs, exactly like kill -9 or a power loss.
    kill -9 $$
fi
mv "$WORK/next" "$HERE/RobloxVersion"
progress update 100 'Update complete' 0 0 false
echo "Roblox updated: $(cat "$HERE/RobloxVersion/.version")"
